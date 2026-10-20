# 第 16 轮：P2 格式 v2（P2a 升版 + P2b 分片归并 / `M_RUN_TRIM`）

> 日期 2026-10-20。编排经用户确认（ask_user）：本轮**只做 P2**，范围 P2a + P2b，`docs/10-design/P2_DESIGN.md` §9 六项全按建议值；稀疏 run（P2c，`M_RUN_HOLE = 8`）只预留 op 号；fsync 出锁另起一轮（范围已定：DISCUSS15 §3 + `durable_vers()`，默认开无开关）。
> 头文件：改前 `backup/r16/fcs.h.pre-r16`（7355 行），改后 `include/fcs.h`（7626 行，diff 841 行）；中间态 `backup/r16/fcs.h.pass4`；P2b 机械改动脚本 `backup/r16/p2b_patch{1,2}.py`（含每处替换的精确计数断言）。
> 回归：`make -j2 -s test` **47 段全绿**（新增 `g16` 6 段；编译 + 全跑 ≈ 4.5 min）、`make -s asan` **`ASAN_EXIT=0`**（21 min 41 s）；日志 `backup/r16/{test,asan}.log`，最终头 `backup/r16/fcs.h.final`（= `include/fcs.h`，与 pass4 只差一行注释）。

**一句话结论**：格式 v2 落地、老库升级路径与分片归并协议全部通过回归；**峰值降幅没有达到设计稿的预估**（layout 200k 峰值 2.60× → **1.90×**，设计稿预估 ≈ 1.5×；800k 1.56× → **1.40×**，预估 ≈ 1.15×；bench4 200k 末态 2.44× → **1.94×**，预估 ≈ 1.45×）。差距的两个来源都量出来了（§4）：一是合并**开始前**的脏源占用（1.3~1.5×，分片管不着），二是分片复用源空间要先"养洞"（N 个源 × 一片的起步长尾）。另外付出的代价是 run 数变多（§4.4），这是取舍，不是 bug。

---

## 1. P2a：格式 v2（`'FCS2'`）

### 1.1 做了什么
| 项 | v1（第 15 轮头） | v2（本轮） |
|---|---|---|
| 超级块魔数 / 头 | `PK_SB`、40 B | `PK_SB2`（'FCS2'）、48 B（`fmt` 字段 = 2，`vers` u64） |
| 清单项 | 40 B（`MENT_V1`），seq/ver u32 | **48 B**（seq/ver u64；`MAN_PER_PG` 102 → 85） |
| SB 内联增量 | `INLINE_MAX_V1 = 32` | **80**（R4 搭车项，§7 只带这一项） |
| 清单页 | 'MANC' | **'MAN2'**；页自带魔数 ⇒ 老库升级后链里新老页混着，`replay_manifest` 逐页分辨 |
| op 号 | … | 新增 `M_RUN_TRIM = 7`（P2b 用）、`M_RUN_HOLE = 8`（**只预留**，P2c） |
| 内存侧 | `seq`/`ver` u32，坑 102/103 的回绕闸 | 全 64 位（`Run::seq/ver`、`snap_version()`、`get_at(k, view)` 的 view、`by_seq`…）；回绕闸保留为"越过 2^32 不回绕"的断言（g13 `seq64` 段、`bench/probe/seqwrap.cpp`） |

### 1.2 老库路径（与设计稿 §4 的差异明说）
- 挂载：两槽按魔数分流；v1 走 `decode_man_v1` / `SB_HDR_V1`，字段加宽零损失。`mounted_format()` 返回挂载那一刻读到的格式（1/2）。
- **升级是惰性的**（设计稿写的是"open 内立即写一份 v2 全量清单 + SB"）：挂载只读内存状态，**第一次写 SB**（第一次持久提交 / 维护产物入账）就是 v2，之后清单链里 v1 页 + v2 页共存直到 checkpoint 重写。原因：open 内多两次 fsync 在"先看看"的场景是白付，而惰性版天然满足"只读不升级"——不需要新的 `read_only` 参数，不提交就什么都不写（`tests/h2.cpp` 的 v1 段验了：只读 → 镜像字节不变；写一次 → `mounted_format()` 重开后变 2）。**不可逆**（用户已拍板）。
- `fmt > 2`：`RC::UNSUPPORTED`（不是 `CORRUPT`，不是当空库格式化）；老代码（第 15 轮头）打开 v2 库：魔数不对 ⇒ 两槽无效 ⇒ CORRUPT（设计稿 §4.4，未改老代码）。
- golden：`tests/golden/{small,bigval,tomb_snap}.{img,kv}` 重出为 v2，老的三件套挪到 `tests/golden/v1/`，`h2` 两套都跑（v2：挂载 + 逐键 + 快照视图 + verify；v1：只读不写回 → 写即升级 → 重开）。覆盖缺口：v1 golden 没有清单页（都是内联增量），"v1 清单链 + v2 页混链"靠 g13 `v1chain` 段（白盒造链 + 掉电）补。

### 1.3 P2a 的坑
| # | 坑 | 处理 |
|---|---|---|
| 1 | `struct SegKey` 要在 `class SegMap` 之前完整定义（64 位化后 SegKey 多了字段，前向声明不够） | 挪位置 |
| 2 | 测试里用 u32 接 `->seq` / 视图号会**静默截断**；printf 用 `%u` 打 u64 | 全改 `%llu` + cast；`-Wformat` 当错看（g15 两处残留本轮一起清） |
| 3 | `vers_ ≥ 2^32` 时 `~0u` 哨兵被当成"当前视图"（pass 3 修）；教训：grep `~0u\b|0xFFFFFFFF` 过一遍 | 哨兵统一 `~0ull` |
| 4 | `memcpy(&Sb, …)` 触发 `-Wclass-memaccess` | `static_cast<void*>` |
| 5 | `commit_delta` 的 `pend_delta_.empty()` 才 `++sb_gen_`（闸代数语义不变），分片的"推迟预留"靠它判断"过了提交点" | 见 §2.3 |

---

## 2. P2b：分片归并 + `M_RUN_TRIM`

### 2.1 协议（实现版，与设计稿 §3.2 的差异加粗）
```
start_job：源集合 S（同第 13 轮）；片区间 [part_lo, part_hi]（§2.2），choose_part 选第一片的大小并预留
merge_job_step：归并 …… 在进入下一个单元格的门口检查：产物页数 ≥ part ⇒ 在**单元边界**切（pcut，不计 yields）
  ⇒ job_land(!final)：finish_run(片 i)（独立 run：自己的 RMETA / bloom / sp；lvl = 目标层；seq = 组内最大 seq —— 同家族同 seq）
     入账（下一个 SB）：M_RUN_ADD(片 i) + 对每个源 r：M_RUN_TRIM(r, trim = 游标所在页, first = 该页首键)
     源 [page0+1, page0+trim) 进 pending_free_（**第 0 页留作身份锚**：RUN_ADD/所有权/重放都按 page0 认 run）
     下一片的预留**推迟**：need_res = true, seal_gen = sb_gen_
  ⇒ 下一次 merge_job_step 看到 sb_gen_ > seal_gen（源前缀已过提交点、进了 free_）才 choose_part + begin_stream
源耗尽：最后一片 ADD + 所有源 DEL（同今天）
```
- **切点在单元边界** ⇒ 一个单元只落在一片里（设计稿允许"跨片边界的单元引用两片"，实现里不会出现），`SMAX` 不变、读路径零改动。
- **TRIM 的内存效果**：`apply_trim_meta(r)`：`nbytes = nbytes0 − pcum[trim]`、`nrec = nrec0 − pncum[trim]`、`first = 新首键`（job_land 设）、`npages` 不变（页号空间不变）；`live_bytes −= 本片里该源的赢家字节（src_vis）`；bloom/sp 不重建（裁掉的键在产物片里，查询会先命中片）。RMETA / RUN_ADD 里写的始终是原始 `first0 / nbytes0`，TRIM 项是增量。
- **重放**：`M_RUN_TRIM` 按 page0 找 run，应用同一个 `apply_trim_meta`；`for_owned` / `free_run_pages` / `verify` 的 owned 集 = 锚页 + `[trim, npages)`；`rebuild_free` 把裁出的前缀算作空闲。
- **作业状态不持久化**（同设计稿）：中途关库 ⇒ 重开后源就是"首键 = k_{i+1} 的短 run"，层规则把它们和同层家族一起再挑出来接着合（g16 `reopen` 段：重开后 4 条裁过的源，继续维护后家族并掉）。
- 墓碑规则改了（见 §2.4 坑 1）：`older_may_contain_grp(k, maxseq)`。

### 2.2 片大小：按洞挑（与设计稿"P = max(1024, 产物/64)"的差异）
设计稿的 P 现在是**上限** `part_hi = max(FCS_PART_MIN_PAGES=1024, 产物估计/64)`；新增**下限** `part_lo = max(FCS_PART_FLOOR_PAGES=64, 产物估计/64)`（⇒ 一个家族 ≤ 64 片）。每次要预留下一片时 `choose_part`：
- 找 `free_` 里 `tail_` 以下**最大的洞** h；若 `h ≥ part_cap(part_lo)`，片 = 能装进 h 的最大片（`part_cap(p) = p + max(p/8, 32) + 8`，二分），夹在 [part_lo, part_hi]；
- 否则片 = **part_lo**，预留落在尾部（起步阶段，见 §4.2 为什么不能退到 part_hi）。
- 产物估计 `< 1024 页`（4 MB）的合并**不分片**（中等产物切成一堆 64 页的小 run 只会让层里 run 数暴涨：layout 800k 实测 L1→L2 的 284 页产物也被切 ⇒ L2 从 31 条涨到 155 条）。
- `set_merge_part_pages(n)`：固定片 n（测试用，g16 全靠它在小负载上逼出分片）；0 = 不分片（老行为）。

为什么不是设计稿的"固定 1024"：pass 3 实测（§3.1 表的"pass 3"列）固定 1024 片**完全没降峰值，800k 反而更差**（1.56 → 1.67，还多了 4 次搬家）。根因在 §4.2。

### 2.3 预留为什么要推迟一个提交点
封口时裁出来的源前缀进的是 `pending_free_`，要过提交点才回 `free_`；封口后**立刻**预留下一片只能在尾部长（pass 2 的 200k 峰值 2.74×，比不分片还高）。所以 `job_land(!final)` 只记 `need_res/seal_gen`，`merge_job_step` 看到 `sb_gen_ > seal_gen` 才预留。推论：**没有提交点就没有复用**——mode 0 / 长时间不提交的后台维护（mode 2/3）里，片在内存里攒到 part 再由 `finish_run` 一次 `alloc_block`（有洞就进洞），峰值回到"活 + 片"，但不再是"活 + 产物"。

### 2.4 P2b 的坑（按发现顺序）
| # | 坑 | 处理 / 证据 |
|---|---|---|
| 1 | **墓碑复活（真 bug，测试逼出来的）**：同 seq 的家族片打破了"组内 seq 连续 ⇒ `SegKey < minkey` 即更老"的坑 94 规则。场景：墓碑 run 与裁过的源同组 + 受害键在更老的 run 里没有副本 ⇒ 老规则判"下面没有副本"把墓碑丢掉，而源里裁剩的旧值还在 ⇒ 复活 | `older_may_contain_grp(k, maxseq)`：按 "seq ≤ maxseq 且不是本组源的**所有** run"（键域 `[first,last]` + bloom）判，不再依赖 seq 顺序 ⇒ 裁过的源（`first` 已前移）与同 seq 家族都算对。g16 `tomb` 段：用老规则的实验头（`build/exp/fcs.h`，`-Ibuild/exp -Iinclude`）跑同一段 **复活 3 个键（52000/52002/52004）**，正式头 0 个 —— 测试有判别力 |
| 2 | 溢出链接管（`add_take`）只允许 `[1, take_lim)`；分片后链的归属要跟着片走 | `j.took` 按片结算；g16 `ovfl` 段（1/3 大值，parts=6，重开 + 附件页过滤 + verify 无所有权错） |
| 3 | `job_land` 会 `delete` 退休的源 Run ⇒ 二步搬家的 `armed` 指针悬空 | 先解除 `armed` 再退休 |
| 4 | `mark_unknown` 不重算 `c->pages`，job_land 慢路径要手动重算 | 慢路径加 recount |
| 5 | **封口后立刻预留只能长尾**（§2.3） | 推迟到 `sb_gen_` 增长后 |
| 6 | **固定 1024 片放不进任何洞**：多源合并裁出的前缀分散在 N 个洞里，每个洞 ≈ 片/N（200k：8 源、max hole 189 页 vs 片预留 1352 页） | 按洞挑片（§2.2） |
| 7 | 退到 `part_hi` 的"没洞就大片长尾"一上来就在尾部要 1160 页，峰值全在这一下（200k 2.34×） | 没洞时用 **part_lo** 长尾（1.94×），再加"产物 < 1024 页不分"（1.90×） |
| 8 | 片越小、源越多 ⇒ run 数越多（200k 末态 runs 11 → 22；2M：30 → **196**） | 取舍：家族 ≤ 64 片、层规则一个家族当一条（merge_run_set 的 `Fam`），读路径按单元不按 run；代价见 §4.4 |
| 9 | `g14 t_hw` 判据失效：默认 tail 本身从 1.9× 降到 1.55×，"回收后比默认低 ≥ 15%"不再成立（12.7%） | 改判策略本身的承诺：回收后 `tail ≤ 1.5× 活页` 且低于默认（测试改法写在 g14.cpp 注释里） |
| 10 | `f3` 的"篡改清单页"段按 'MANC' 找页 ⇒ v2 库里找不到 ⇒ FAIL（首次全量回归才暴露：单跑没覆盖 f3） | 测试改为认 'MAN2' / 'MANC' 两种魔数（老库混链） |
| 11 | g16 测具：mode 1 大预算下作业在一个提交里跑完 ⇒ 提交后采样不到"裁过的源"（不是 bug）；"复活"判定要排除被重写的受害键；随机键负载下低层 run 几乎含所有键 ⇒ 要唯一键 + `set_lvl_t(8)` 才能暴露老墓碑规则 | 测试按此设计（`build_midjob(uniq=true)`、受害键 50000 起、后续只写 1..20000） |

---

## 3. 实测（全部同机同次；layout / bench4 的空间数字确定性复现，延迟取 3 次中位）

### 3.1 layout（`build/layout N 500 32 1 1 0 pct`；老头 = `backup/r16/fcs.h.pre-r16`，编法 `g++ -std=c++17 -O2 -pthread -Ibuild/old -Iinclude bench/probe/layout.cpp -o build/layout_old`）
| 负载 | 老头 末态 / **峰值** | pass 3（固定 1024 片 + 推迟预留） | **最终**（按洞挑，floor 64） | floor 32（`-DFCS_PART_FLOOR_PAGES=32`） | 设计稿预估 |
|---|---|---|---|---|---|
| 200k pct 0 | 2.48 / **2.60×** | 2.48 / 2.48，moves 0 | 1.90 / **1.90×**，moves 0，runs 22 | 1.84 / 1.84，runs 25 | ≈ 1.5× |
| 200k pct 150 | 1.32 / **2.61×** | 1.27 / 2.50 | 1.44 / **1.77×**，reclaims 7 | 1.41 / 1.72 | — |
| 800k pct 0 | 1.53 / **1.56×** | 1.67 / 1.67，**moves 4** | 1.37 / **1.40×**，moves 0，runs 43 | 1.33 / 1.37，runs 56 | ≈ 1.15× |
| 800k pct 150 | 1.55 / **1.83×** | 1.60 / 1.68，moves 4 | 1.46 / **1.46×**，moves 2 | 1.38 / 1.38 | — |
| 2M pct 0 | 1.15 / 1.15×，runs 30 | — | 1.15 / 1.15×，**runs 196** | — | — |

块层写量不变（200k 41.7 → 41.8 MB；800k 169 → 170 MB）。默认 floor 取 64 不取 32：峰值只差 0.03~0.06×，run 数多 15~30%。

### 3.2 bench4 200k（`build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 {0,150}`，×3）
| | 老头 pct 0 | **新头 pct 0** | 老头 pct 150 | **新头 pct 150** |
|---|---|---|---|---|
| space（hw） | 22.0 MB / 活 9.0（**2.44×**） | **17.5 MB / 活 9.0（1.94×）** | 13.3 MB / 活 9.5（1.40×） | 13.2 MB / 活 8.9（1.49×） |
| 块层写 | 18.03 MB | 17.98 MB | — | — |
| insert / update / get µs/op（中位） | 0.68 / 2.38 / 0.57 | 0.67 / 2.14 / 0.52 | 0.67 / 2.58 / 0.67 | 0.73 / 2.72 / 0.54 |
| range µs/scan | 4.42 | 4.28 | 6.53 | 4.62 |
延迟差在噪声内（2 核沙箱）。pct 150 的末态两头都在阈值内（回收策略的目标是 ≤ 1.5×），不比绝对值。

### 3.3 g16（`build/g16`，1.6 s）
分片基本面 parts=201 trims=673 job_fast=27540 job_slow=11，tail 超出活页峰值 1489 页，清账后 runs=46（家族并掉、裁过的源 0 条）；跨重开（重开后裁过的源 4 条）；abort + rollback(快照)；快照作废；溢出链 parts=6；墓碑复活 0（老规则实验头 3）。

---

## 4. 为什么没到预估，以及这个方案的边界

### 4.1 峰值的两块
以 200k 为例（`build/layout 200000 500 32 1 0 0 0` 的逐提交轨迹）：L2→L3 合并开始时 tail 2906 页、活 1925 页 ⇒ **合并还没开始就是 1.51×**（8 条 L2 源各 25% 脏 + L0/L1）。老头在此之上一次预留 ≈ 产物 1.1 倍（+1900 页）⇒ 2.60×；新头 +760 页 ⇒ 1.90×。800k 同理：开始前 10233/7911 = **1.29×**（31 条 L2 源各 2/3 脏 —— 4·lvl_t 悬崖让它们攒着），老头 +2115 页，新头 +894 页。设计稿预估时把"开始前"当成 ≈ 活，所以 1.45× / 1.15× 从一开始就不可能；**分片只能压合并自己那一块**，压掉了 55~60%。开始前的脏源占用是层规则 / 合并时机的事（lvl_t、4t 悬崖、(a)/(d) 的脏度阈值），不在本轮范围。

### 4.2 合并自己那一块为什么不是 ≈ 一片
封口释放的是**各源的前缀**：N 个源就是 N 个洞，每个洞只有产物消耗量的 1/N。片要落进洞，洞得先 ≥ `part_cap(片)`，在那之前每一片都只能长尾（"养洞"）。起步长尾 ≈ N × 活比 × 1.1 × 片：200k（N=8，活 0.75）≈ 6.6 片 ⇒ floor 64 时 ≈ 10 片 × 104 页 ≈ 700 页（实测 +760）。固定 1024 片时 ≈ 6.6 × 1160 = 7700 页 > 整个产物 ⇒ 一点复用都没有（pass 3 的结果）。所以：**小 DB、多源、源没分过片 ⇒ 片必须小才有用**，而小片 = 多 run。这个起步量与 DB 大小无关（只跟 N、片、活比有关）：1 GB 的合并它还是 ≈ 3 MB，所以在大库上分片的收益接近设计稿的"≈ 一片"；在 8 MB 的探针库上它就是 10%。

### 4.3 规模化时会怎样（分析，**未实测**）
源本身是分过片的家族时（L3 以上），裁掉的不再是"前缀碎片"而是**整片**（锚页除外，洞 = 片 − 1 页），且同一时刻只有 t 个家族在前沿 ⇒ 起步 ≈ t × 活比 × 1.1 片，与家族片数无关 —— 这正是 LSM 分区压实的动力学，大合并的峰值 ≈ 活 + 常数片。两个注意点：① 锚页让洞比源片少 1 页，`part_cap` 带 p/8 余量 ⇒ 下一层的片会比上一层小 ≈ 12%（64 片的下限止住，不会无限缩）；② 归并宽度 = 源 run 条数（家族展开），≤ t × 64（4t 悬崖时 ≤ 4t × 64），每条源一个游标页（4 KB）与一条 TRIM/片 —— 第 17 轮以后值得做"家族当一条虚拟源顺序读"的游标。

### 4.4 代价与已知保留
- **run 数**：每个分过片的家族 ≤ 64 条 run；层规则按家族计数（不会误触发再合并），点读 / 范围读按单元（每单元仍只有一片的段）不受影响；受影响的是 O(runs) 的遍历（merge_run_set、回收规划器、verify、挂载 O(runs)）与归并宽度。2M 探针末态 196 条 run（老头 30）。
- **清单量**：每片 1 条 ADD + 每源 1 条 TRIM；31 源 × 20 片 = 620 条 ≈ 8 页清单，只写一次。
- **mode 0 / 不提交就不复用**（§2.3）；**长快照**：裁出的前缀与今天的源页一样进 `deferred_`，峰值回到老样子（P2_DESIGN §5.4）；`hw_reclaim_pass` 对 `trim > 0` 的 run 跳过（裁过的源不搬，等家族并掉）。
- `stream_cap_est` 对 `Σ 源页 < 512` 不预留（老规则）：尾片走内存 + `alloc_block`。
- 没做：P2c 稀疏 run（op 号已留）；fsync 出锁（下一轮）；"开始前"的脏源占用（层规则）。

---

## 5. 复现命令
| 目的 | 命令 | 时间 |
|---|---|---|
| 老头对照副本 | `mkdir -p build/old && cp backup/r16/fcs.h.pre-r16 build/old/fcs.h`；编老头探针加 `-Ibuild/old -Iinclude`（顺序决定命中哪份头） | — |
| layout 峰值 | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout && build/layout 200000 500 32 1 1 0 0`（`== after update:` 行的 `peak`/`moves`/`runs`）；800k ≈ 10 s；2M ≈ 55 s | 2 s |
| 逐提交轨迹 | `build/layout 200000 500 32 1 0 0 0 \| grep -aE "\(\+[0-9]{3,}\)"`（quiet=0：每次 tail 抬升一行，含 holes/max hole/各层 run 数） | 2 s |
| 分片决策轨迹 | 编译加 `-DFCS_TRACE`，`stdbuf -o0 -e0 build/layout_tr … 2>&1 \| grep -a "JOB choose_part\|JOB reserve\|JOB part"` | 90 s（trace 很多） |
| floor 32 变体 | `g++ … -DFCS_PART_FLOOR_PAGES=32 bench/probe/layout.cpp -o build/layout_f32` | — |
| bench4 | `g++ -std=c++17 -O2 -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3 && build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0`（`# fcs space:` 行） | 2 s |
| g16 单段 | `build/g16 {parts,reopen,rollback,snap,ovfl,tomb}`；老墓碑规则对照需要 `build/exp/fcs.h`（实验头，可从 pass4 头把 `older_may_contain_grp` 换回 `older_may_contain` 复现） | 1.6 s |
| 64 位回绕 | `build/g13 seq64`、`build/seqwrap 50 400` | 1 s |
| golden | `build/h2`（v2 三件套 + v1 三件套） | 11 s |

---

## 6. 文件清单
- `include/fcs.h`：P2a（常量 @60~140、`Sb`/`ManEnt` 编解码 @≈2920~2980、`read_sb`/`write_sb`/`mount` @≈3580~3690、`replay_manifest` @≈3820）；P2b（`MergeJob` 尾部字段 `part/part_lo/part_hi/src_vis/need_res/seal_gen`、`part_cap_of/choose_part`、`start_job`、`merge_job_step` 的 pcut 与推迟预留、`job_land(!final)`、`apply_trim_meta`、`trim_ent`、`older_may_contain_grp`、`merge_run_set` 的 `Fam`）。
- `tests/g16.cpp`（新，6 段）、`tests/h2.cpp`（重写：v2 + v1 golden）、`tests/g13.cpp`（`seq64`/`v1chain`/`cut` 改写）、`tests/g14.cpp`（t_hw 判据）、`tests/g15.cpp`（格式告警）、`tests/golden/` + `tests/golden/v1/`、`bench/probe/seqwrap.cpp`（64 位）。
- `Makefile`：TESTS + asan 列表加 g16，h2/g13 段文案。
- 备份：`backup/r16/{fcs.h.pre-r16, fcs.h.pass4, g16.cpp, p2b_patch1.py, p2b_patch2.py, test.log, asan.log}`。

## 7. 回归
- `make -j2 -s test`：**47 段全绿**（`backup/r16/test.log`）。第一次全量跑在 f3 停下（§2.4 坑 10：清单页魔数），到那里 218 s（含全部编译）；修 f3 后重跑 47 段 44 s（二进制已编好）⇒ 编译 + 全跑 ≈ 4.5 min，与第 15 轮持平。
- `make -s asan`（address + undefined，含 g16；tsan w1/g7/g11/g13/g14）：**`ASAN_EXIT=0`**，21 min 41 s（1301 s；第 15 轮 19 min 53 s），无 AddressSanitizer / UBSan / LeakSanitizer / ThreadSanitizer 报告（`backup/r16/asan.log`）。
- 本轮单跑过的分段：g16 ×6、g13 全量、g14 全量、h2、g6、g15、f1、f3、seqwrap；A/B 对照全部用 `backup/r16/fcs.h.pre-r16` 编的老头二进制。
