# 第 17 轮：fsync 出锁 + 合并时机规则 + 家族"虚拟源"游标（+ 中等产物按洞分片）

> 日期 2026-10-20。编排经用户拍板：三项**全部处理**——① fsync 出锁 = `docs/30-discuss/DISCUSS15_FSYNC.md` §3 + `durable_vers()`，**默认开、无开关**（组提交 §4.5、双内存表 §3.5 不做）；② 合并时机规则（压"合并开始前"那 1.3~1.5×）；③ 家族"虚拟源"游标（压归并宽度）。
> 头文件：改前 `backup/r17/fcs.h.pre-r17`（7626 行 = 第 16 轮定稿），① 定稿 `backup/r17/fcs.h.step1`（7742 行，diff 305 行），本轮定稿 `include/fcs.h`（7851 行，对 pre-r17 的 diff 580 行）= `backup/r17/fcs.h.step2`。机械改动脚本 `backup/r17/{fsync_patch1~3,timing_patch1,famcur_patch1,holefit_patch1}.py`（每处替换带精确计数断言）。
> 回归：`make -j2 -s test` 全绿两次（① 之后 `backup/r17/test1.log`；定稿 `backup/r17/test2.log`，48 段，含新增 `g17` 8 段）；`make -s asan` 见 §6（`backup/r17/asan.log`）。

**一句话结论**：① 做到了——提交者的 fsync 不再挡读者（慢盘探针单次 get 79 ms → 0.01 ms；`stall` 读者 max 3.9~7.7 → 3.3~3.7 ms、p99.9 68~78 → 47~52 µs），语义不变，坑全列在 §1.3。② 是**否定结论**：第 16 轮猜的"脏 L2 攒着等 4t 悬崖"不成立，所有简单时机旋钮（悬崖 2/3、门槛 25%、大前缀 30%）实测都比老规则差 ⇒ **默认不变、旋钮保留**；调研时抓到的真正元凶（大合并刚结束时落尾的那条 <1024 页、不分片的中等产物）顺手治了——"中等产物按洞分片"（§2.4），layout 200k 峰值 1.96~2.00× → **1.75~1.81×**，800k 1.40~1.49× → **1.36~1.37×**，零写放大。③ 做到了且顺带修了一个**正确性 bug**：老头归并源 > 64 条时 `live` 账漂移（famw 400k/16/8 老头 verify CORRUPT 851 处 → 新头 OK），游标数从"源 run 数"降到"家族数"（151 → 8）；但在 layout / bench4 的默认负载里 L3 家族从不互并（源 ≤ 32、游标 = 源），③ 的**收益只在小片负载上可见**，耗时 −2~7%。

---

## 1. ① fsync 出锁（DISCUSS15 §3 的实现版）

### 1.1 做了什么
提交 `commit()` 从"XLock 内一气呵成"拆成三段（`commit_phase1/2/3`，fcs.h ≈L1888 起的注释块是权威）：

| 段 | 持锁 | 做什么 | 耗时量级 |
|---|---|---|---|
| 阶段一 `commit_phase1` | XLock 内 | `ensure_ready` / 组提交短路 / `flush_mem`（编码 + 发写）/ 清单内存更新 + 清单页 pwrite / **抓 SB 载荷**（`sb_capture`：字节 + 版本 + 代数）；发一张提交票 `cseq_` | 内存 + 发写，无等待 |
| 阶段二 `commit_phase2` | **XLock 外**，`cmu_` 顺序器按票号串行 | `wait_io` → sync → 写 SB（`sb_flush`）→ sync。失败 ⇒ 粘性 `cfail_` | 两次 fsync（沙箱 0.25 ms p50，慢盘 ms~几十 ms） |
| 阶段三 `commit_phase3` | XLock 内 | 发布 `sb_synced_` / `durable_vers_` / `durable_gen_`，`promote_pending`，R7 自动归一 | µs |

- **两个代数**（DISCUSS15 §2.3）：`sb_built_`（抓过载荷的 SB 次数，锁内 +1）与 `sb_synced_`（其中已落盘的最大代数）。页复用闸 `pend_need_gen_` / `def_need_gen_` 现在比的是 `sb_synced_`（"在飞的 SB 不算"，fcs.h ≈L4258 / L4282）；单线程时两者相等，退化成老的单一代数。
- **提交顺序器**：票号在 XLock 内发；阶段二在 `cmu_` 内按票号串行 ⇒ 盘上 SB 的先后 = 票号先后；`cdone_` 原子推进，`drain_inflight()` 是"在飞提交等待点"。
- **持锁调用方走内联版** `commit_locked`：`maint_commit` / `abort` / `snapshot` / NOSPACE 重试的中间提交点三段连着做、不放锁（语义与老头相同）。
- 内联写 SB 的路径（`write_sb`：rollback / shrink / 维护提交点）、`reload`、析构之前都先 `drain_inflight()`。
- `snapshot()`：在飞提交先排空再抓（快照号 = 持久后的版本），与 DISCUSS15 §4.3 一致。
- **`durable_vers()`**：已落盘的最新 SB 版本（原子，acquire）。`commit()` 返回前 `durable_vers() == last_commit()`；别的线程在某次提交的阶段二期间读到 `durable_vers() < last_commit()` 就是"看到的新视图还没持久"。
- **语义不变**：`commit()` 返回 ⇒ 持久；读者可见性本来就是"可见先于持久"（未提交的 `mem_` 读者就能看到）。变宽的只有失败窗口：阶段二失败时内存已切到新视图、盘上还是旧提交点 ⇒ `dirty_` + 按盘重建，窗口从 µs 变成一次 fsync 的 ms 级（DISCUSS15 §2.4 说过）。
- 槽号在**写的时候**翻（不在抓载荷时翻）：写失败槽不翻，下一次仍写同一槽，不会把唯一有效的 SB 擦掉。

### 1.2 实测（全部同机；老头 = `backup/r17/fcs.h.pre-r17`，编法 `-Ibuild/r16 -Iinclude`）
| 探针 | 老头 | 新头 | 说明 |
|---|---|---|---|
| `bench/probe/fsync_block.cpp`（`fsync_block 40 5`：sync 睡 40 ms，写者 commit 时主线程 get） | get avg **79.26** / max 79.31 ms（= 2×40：锁内两次 sync） | avg **0.01** ms | 探针只用公开 API，新老头都能编 |
| `build/stall 100000 100 4 0`（×3，读者单次 get） | max 3.9 / 7.7 / 6.5 ms；p99.9 68~78 µs | max **3.3 / 3.5 / 3.7** ms；p99.9 **47~52** µs | 残余 max 是维护单元（≤ 12.5 ms 预算），不是提交 |
| `FCS_PROF=1` 提交分解 | `commit_delta` 锁内 0.59 ms | **0.00** ms（SB 写 + sync 在锁外） | 锁内剩 `flush_mem`（编码 + 发写） |
| g17 `t_lockfree_sync`（sync 睡 40 ms × 2，窗口内 get） | — | get max 0.01~0.04 ms | 测试 |
| TSan（`make tsan`，g17 60 轮） | — | PASS | 3 写者 × mode 3 × 2 读者 |

**没做 / 没量的**：组提交（§4.5）与双内存表（§3.5）按拍板不做 ⇒ 锁内的 `flush_mem` 还在（DISCUSS15 §1 量过：2000 条 × 32 B 几百 µs），这是现在读者停顿的地板；本沙箱 fsync 本来就快（0.25 ms），① 的收益在真机慢盘上才是数量级的（fsync_block 用睡眠模拟）。

### 1.3 ① 的坑（按发现顺序，全部已修并有测试）
| # | 坑 | 处理 |
|---|---|---|
| 1 | 阶段二失败 → `reload` 之后，下一次 `commit()` 若"没有可写的东西"走短路，盘上 SB 仍是旧的 | 短路条件加 `!need_sb_`（抓过载荷却没落盘 ⇒ 必须再写 SB）；g17 `t_phase2_fail` |
| 2 | "无在飞提交"短路没看 `cfail_`：前一张票失败了，后一张空提交会返回 OK | 短路里查 `cfail_` ⇒ `dirty_` + `RC::IO` |
| 3 | 空票（不抓载荷）在阶段三发布什么版本？用 `vers_` 会把**还没持久**的版本当持久 | 用 `cap_vers_last_`（最近一次抓载荷的 SB 版本） |
| 4 | `open()` 没复位顺序器：上一会话的票号 / `cfail_` 带进新会话 | `open()`：`drain_inflight()` + `cfail_=false` + `cap_vers_last_=0` + `need_sb_=false`；代数不复位（`pend_need_gen_` 只会比它小） |
| 5 | `reload` 先复位 `cfail_` 再排空 ⇒ 一张还没轮到的旧票把新状态的 SB 写回去 | 先 `drain_inflight()` 再复位 |
| 6 | `last_commit()` 裸读 `vers_`，测试里与 `durable_vers()` 并发比较会读到撕裂 / 乱序 | 测试只在 commit 返回后比较；文档明说 `durable_vers()` 是唯一的跨线程持久自检 |
| 7 | 析构时别的线程还在阶段二 | 析构先 `drain_inflight()`；"不并发析构"仍是调用方责任（老头同样） |

**残余窗口（老头同样，未修）**：SB 写失败 → `reload` 按盘重建会把"被换掉的清单页"当空闲——老头在锁内写 SB 失败时同样如此；修法是失败后把那些页压进 `pending_free_` 等下一次成功的 SB，本轮没动。

---

## 2. ② 合并时机规则：否定结论 + 顺手治了真正的元凶

### 2.1 现行规则（第 13 轮起，本轮只是把硬编码变成旋钮）
家族 = 同层同 seq 的 run 组。某层家族数 fam ≥ t（`lvl_t`，默认 8）⇒ (a) 最老 k 条垃圾 ≥ `lvl_dirty_pct_`（30%）或 (c) 总页 ≤ `merge_cheap` ⇒ 合 k 条；否则 fam ≥ cliff·t（**4t**）⇒ 无条件合 4t 条；(d) 任意 1~t 的前缀垃圾 ≥ 50%（≥ `FCS_LVL_BIG_PAGES`=1024 页的前缀按 `lvl_dirty_big_pct_`，**默认 50 = 老行为**）⇒ 合 / 重写；组页 > `merge_budget_` ⇒ `start_job`（分片作业）；产物 ≥ 1024 页才分片。
新旋钮：`set_lvl_cliff(n)` / `set_lvl_dirty_pct(p)` / `set_lvl_dirty_big_pct(p)`，编译期默认 `FCS_LVL_CLIFF` / `FCS_LVL_DIRTY_PCT` / `FCS_LVL_DIRTY_BIG_PCT`。

### 2.2 调研：第 16 轮的猜想不成立
ROUND16 §4.1 把"合并开始前的 1.3~1.5×"归给"脏 L2 攒着等 4t 悬崖"。逐提交轨迹（`build/layout 800000 500 32 1 0 0 0`）看到的是：
- 800k 撞悬崖做 32 宽合并时，L2 整体只 **~20% 脏**（不是 50%）；合并后最大洞 220 页 < 新 L2 产物 310 页 ⇒ 产物落尾 ⇒ 峰 1.40×。
- 200k：最大洞 262 < 产物 288 ⇒ 1.97×。
- 把 L2 的脏门槛降到 20% 去"提前合"：写放大 3.14×（老 2.3×），峰值没降。
⇒ 峰值的主因是 **分片产物把洞打碎 + 大合并刚结束时落下来的中等产物（< 1024 页、不分片、整体落尾）**，不是"攒着等"。

### 2.3 A/B（`backup/r17/timing_ab.log`；`build/layout_<变体> N 500 32 1 1 0 pct`，列 = 峰值× / 末态高水位× / 设备写 MB）
| 变体 | 200k pct0 | 200k pct150 | 800k pct0 | 800k pct150 | 2M pct0 |
|---|---|---|---|---|---|
| 老规则（big50 / cliff4 / pct30） | 1.98 / 1.98 / 41.8 | 1.77 / 1.43 / 45.1 | 1.40 / 1.36 / 170.8 | 1.55 / 1.55 / 172.7 | 1.15 / 1.15 / 439.1 |
| big30（大前缀 30% 就重写） | **2.38** / 2.33 / 42.1 | — | 1.37 / 1.35 / 168.6 | — | 1.46 / 1.23 / **499.3**（写 +14%） |
| big30 + cliff2 | — | — | **1.62** / 1.62 / 191.3 | — | 1.09 / 1.09 / 417.3（runs 509） |
| big50 + cliff2 | — | — | 1.35 / 1.35 / 178.4 | — | — |
| big30 + pct25 | 2.32 | — | 1.57 / — / 185.7 | — | — |
| big30 + cliff3 | — | — | 1.43（runs 109） | — | — |

- big30 在 200k **变差**的机理（`build/l200_big30.txt`）：(d) 触发的重写产物 ≈ 920~1060 页 < 1024 不分片 ⇒ 整产物落尾（#512 +1060、#571 +1023）。
- cliff2 把 run 数翻倍、写量 +12%，峰值反而高（更多更小的家族 ⇒ 更碎的洞）。
- **mode 1 有 20 ms 维护时间预算 ⇒ 布局不确定、运行间噪声**：同头 800k 峰值 1.40× 与 1.53× 都出现过、2M runs 189 vs 380。A/B 只信 > 0.1× 的差异；上表关键格取 2~3 次中位。
- **决定：默认不变**（cliff 4 / pct 30 / big-pct 50），三个旋钮保留给特定负载调参。ROUND16 §4.1 的那句归因作废，以本节为准。

### 2.4 顺手治元凶：中等产物按洞分片（`set_part_hole_fit`，默认开）
`start_job`：产物估计 `lbp < FCS_PART_MIN_PAGES(1024)` 以前一律不分片。现在：若 `lbp ≥ 4·FCS_PART_FLOOR_PAGES(256)` 且**最大洞装不下整个产物**（`largest_hole() < part_cap_of(lbp)`）⇒ 取最大的片 p 使 `part_cap_of(p) ≤ 最大洞`（二分），只要 `4p ≥ lbp`（2~4 片）就按固定片 p 切（`j.part_lo = j.part_hi = p`）。片 ≥ 64 页；不够 ⇒ 照旧不分片。计数 `part_hole_fits()`。
- 它治的正是 §2.2 抓到的那一跳：200k 最大洞 ≈260~280 / 产物 ≈288 ⇒ 切成 ≈240 + 几十页两片，第一片进洞、尾巴进小洞（确定性复现里末态 L2 = `20 + 242` / `27 + 236` 两片）。
- **实测**（`build/layout N 500 32 1 1 0 pct`，各 2 次；`build/layout_nohf` = `-DFCS_PART_HOLE_FIT=0`）：

| 负载 | 关（老行为） | 开 | 设备写 |
|---|---|---|---|
| 200k pct0 峰值 | 2.00 / 1.96× | **1.75 / 1.81×** | 41.8 = 41.8 MB |
| 200k pct150 峰值 / 末态 | 1.86× / 1.55× | 1.86~1.87× / **1.29~1.37×** | 44.3 = 44.3 MB |
| 800k pct0 峰值 | 1.41 / 1.40× | **1.36 / 1.37×** | 170.2~170.5 MB 同 |
| 800k pct150 峰值 | 1.49 / 1.40× | **1.36 / 1.37×** | 同 |
| 2M pct0 峰值 | 1.15×（第 16 轮基线） | 1.15×，devw 442.8 MB（噪声内：同头 427.8~439） | hole_fits=6 |
| bench4 200k（§5 命令）pct0 / pct150 末态 | 17.51 MB / 13.23 MB | **17.51 / 13.23 MB（不变，hole_fits=0）** | 时间型有序键的产物要么 ≥ 1024 页要么有洞装得下 ⇒ 从不触发 |

- 代价：run 数 +1~2（200k 21→23、800k 43~48→49~54），第二片常是几十页的小尾巴（与 ≥1024 路径的最后一片同性质）。
- 确定性复现：g17 `t_hole_fit` 用 `set_maint_budget_us(0) + set_maint_units_max(8)`（无时间预算 ⇒ 布局确定；每提交 8 单元 ⇒ 归并跨多个提交点，裁掉的源前缀能过提交点变洞），开 1.82× / 关 1.97×，开 ⇒ 触发 1 次且峰值 ≤ 关。
- **坑（回归抓到的）**：第一次全量回归 g13 (f)（`lvl_t=2` 纯插入 60 批，断言"顶层 ≤ 2 条 run"）FAIL：顶层那条 ≈430 页的产物被按洞切成 4 片 ⇒ 4 条 run、1 个家族。层规则早就按家族计数（第 16 轮），测试没跟上 ⇒ 改成"顶层**家族** ≤ 2 且 run ≤ 8"（`fams_at`）；不是引擎问题，但说明"run 数"这个观测量从第 16 轮起就要按家族看。
- **坑**：`budget 0` + 默认 256 单元会让整个 L2→L3 归并在**一个提交内**做完，释放页全卡在 `pending_free_`，1584 页产物整体落尾 ⇒ 2.32×（比默认还差）。这不是新问题，是"维护全挤在一个提交点"的固有代价，写在这里免得以后拿 budget 0 当基线。

### 2.5 峰值现状与边界（对 ROUND16 §4 的更新）
200k 1.75~1.81× / 800k 1.36× / 2M 1.15×。剩下的部分：200k 的 1.75× 里约 0.4× 是插入期 L1 run 把每次 L1→L2 释放的洞吃掉、L2 产物只能顺序落尾（§2.4 轨迹 `ins#64…#384` 每 64 提交 +311/+352），这是"分片复用要先养洞"的那一块（ROUND16 §4.2），改时机规则治不了；真要再压得动 `alloc_pages` 的策略（新 L1 run 不吃大洞）或 P2c 稀疏 run，都是另一轮的事。

---

## 3. ③ 家族"虚拟源"游标

### 3.1 问题
P2b 之后一个家族（同层同 seq）是若干片；`merge_emit` 对**每条源 run** 开一个游标，L2→L3 归并的源 = t 个家族 × 每家几十片 ⇒ 游标可达 t×64；更要命的是 `merge_job_step` 的源下标用 64 位掩码 ⇒ **源 > 64 条时 `live` 账漂移**（老头 famw 400k/16/8：verify "live mismatch" 851 处；layout / bench4 的默认负载源 ≤ 32，所以第 16 轮没暴露）。

### 3.2 做了什么
- `build_chains`（fcs.h ≈L3544）：源按 seq 分组；同 seq 且键域两两不相交（按 first 排序后 `first_i > last_{i-1}`）的 run 串成一条**链**（理论上同 seq 只来自分片 / 搬家 / 裁源，总是不相交；不满足就各自单链 = 老行为）。只收与本片 `[lo, hi)` 相交的 run。
- `Cur` 多了 `chain / ci / slo / shi / open_chain / next_run`：一条链一个游标，片耗尽就 `next_run` 换下一片；`klo = max(slo, r->first)`——因为 (d) 单源重写的产物与裁过前缀的源**同 seq**（坑 1）。
- `merge_job_step`：源下标不再限 64 条——每个单元记"在本单元有段的源下标"（有序小表，≤ 家族数），源 run → 下标用哈希表（`sidx_of`，以前逐条线性找）。
- `MaintStats` 多 `merge_src_max / merge_cur_max`（一次归并最多的源 run 数 / 游标数）；`FCS_R17` 特性宏给探针 / 测试选分支。

### 3.3 实测（`bench/probe/famw.cpp`：`famw N B part lvl_t budget`，小片负载让 L2→L3 的源 = 家族 × 很多片；新老头都能编）
| 负载 | 老头 | 新头 |
|---|---|---|
| `famw 400000 500 16 8 64` | **verify CORRUPT（851 处 live mismatch）**，6.7 s，level_us 5.7 s | verify **OK**，src_max=151 **cur_max=8**，6.6 s，level_us 5.5 s，dev read 236 MB 同 |
| `famw 400000 500 48 4 64` | 2.9 s | src_max=28 cur_max=4，2.7 s（−7%） |
| `famw 200000 500 16 4 64` | verify OK（源 72 时账恰好没漂） | src_max=72 cur_max=4 |
| `build/layout 2000000 500 32 1 1 0 0` | — | src_max=32~35 cur_max=32：L2 单 run 互并、无家族片 ⇒ 游标 = 源，③ 不起作用 |
| g17 `t_wide_family`（N=200k, lvl_t 4, part 16, budget 64） | — | runs=122 src_max=72 cur_max=4 slices=1061，verify / 账零漂移 / 真值 / 重开全对 |

**明说**：③ 的 CPU / 读量收益在默认参数下几乎量不到（家族互并时 L3 家族片之间本来就不交错，游标多只多了堆的 log 项）；它真正的价值是 (1) 修了 >64 源的账漂移（正确性），(2) 把归并宽度与片数解耦，为更小的片（P2c / floor 32）扫清了路。

### 3.4 ③ 的坑
| # | 坑 | 处理 |
|---|---|---|
| 1 | 家族片键域不相交，但 (d) 单源重写产物与裁过前缀的源同 seq 且**键域重叠**（源的 [trim, …) 与产物） | 链内键下界 `klo = max(slo, r->first)`；`build_chains` 检查两两不相交，重叠就拆成单链 |
| 2 | 源下标必须按 run 记（不能按链）：`src_vis` / `live` 扣减是按源 run 的 | `sidx_of` 哈希 + 单元级有序小表 |
| 3 | 老头 `merge_job_step` 源 ≥ 64 ⇒ 掩码溢出 ⇒ 账漂移（见 §3.3） | 本轮顺带修掉；g17 `t_wide_family` 断言 `src_max > 64` 且账零漂移 |
| 4 | `t_wide_family` 最初 N 160k / lvl_t 8 跑出 src_max=8（家族没攒够）⇒ 测试空转 | 改 N=200k / lvl_t 4 / part 16 / budget 64 |

---

## 4. 回归与新增测试
- `tests/g17.cpp`（8 段，≈5.4 s）：`t_basic`（durable_vers == last_commit）、`t_lockfree_sync`（慢 sync 下读者不被挡）、`t_empty_ticket`（空票等持久）、`t_phase2_fail`（阶段二失败粘性 + reload + need_sb_）、`t_snapshot_inflight`（在飞时快照）、`t_multi_writers`（3 写者 × mode 3 × 2 读者，TSan 也跑）、`t_wide_family`（③）、`t_hole_fit`（②(c3)，开 / 关各一遍）。
- `tests/g13.cpp` (f)：顶层判据 run 数 → 家族数（§2.4 的坑）。
- `make -j2 -s test`：48 段全绿（`backup/r17/test2.log`，≈5.5 min；第一次跑在 g13 (f) 停下，改判据后重跑全绿）；`make -s asan`：§6。

## 5. 复现命令
```
# 老头对照
mkdir -p build/r16 && cp backup/r17/fcs.h.pre-r17 build/r16/fcs.h      # 老头编译加 -Ibuild/r16 -Iinclude（include 顺序决定命中哪份）
# ①
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/fsync_block.cpp -o build/fsync_block && build/fsync_block 40 5
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/stall.cpp -o build/stall && build/stall 100000 100 4 0      # ×3 取中位
# ② A/B（变体 = -DFCS_LVL_DIRTY_BIG_PCT=30 -DFCS_LVL_CLIFF=2 -DFCS_LVL_DIRTY_PCT=25 的组合）
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout
g++ -std=c++17 -O2 -pthread -DFCS_PART_HOLE_FIT=0 -Iinclude bench/probe/layout.cpp -o build/layout_nohf
build/layout 200000 500 32 1 1 0 {0,150}; build/layout 800000 500 32 1 1 0 {0,150}; build/layout 2000000 500 32 1 1 0 0   # 2 s / 6~15 s / 40~60 s，各跑 ≥2 次
g++ -std=c++17 -O2 -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 {0,150}          # 看 "# fcs space" / "# fcs r17" 两行
# ③
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/famw.cpp -o build/famw && build/famw 400000 500 16 8 64    # ≈7 s；老头同参 verify CORRUPT
```

## 6. asan
`make -s asan > backup/r17/asan.log`（13:29 → 13:51，≈22 min）：**`ASAN_MAKE_EXIT=0`**，日志里 0 处 `ERROR: AddressSanitizer` / `LeakSanitizer`；g17 在 ASan 下 PASS（21.1 s），TSan 构建（g17 60 轮）PASS（35.1 s）。

## 7. 文件清单
- `include/fcs.h`：①②③ + c3；`FCS_R17`；新旋钮 `set_lvl_cliff / set_lvl_dirty_pct / set_lvl_dirty_big_pct / set_part_hole_fit`；新观测 `durable_vers() / part_hole_fits() / MaintStats::merge_src_max, merge_cur_max`。
- `tests/g17.cpp`（新，Makefile 已登记 test / asan / tsan）。
- `bench/probe/fsync_block.cpp`、`bench/probe/famw.cpp`、`bench/probe/hfprobe.cpp`（新）；`bench/probe/layout.cpp`（多一行 `== merge width … hole_fits=`）；`bench/bench4.cpp`（多一行 `# fcs r17:`）。
- `backup/r17/`：头的三个快照、补丁脚本、`test1/test2.log`、`timing_ab.log`、`asan.log`。
