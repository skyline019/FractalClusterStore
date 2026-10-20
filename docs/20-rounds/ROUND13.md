# 第 13 轮执行与收尾（2026-10-20）

范围 = `docs/EVAL12.md` §6 弱点清单的前五项（① seq 重编号 ② 产物流式落盘 ③ 读放大触发的层合并 ④ mode 1 预算 / mode 3 线程 ⑤ 顶层按脏度），
不含 P2 格式升版（仍单开一轮）。改前头 `backup/r13/fcs.h.pre-r13`，改后头 `backup/r13/fcs.h.final`（= `include/fcs.h`）。

**收尾状态**：`make -j2 -s test` **44 段全绿**（`build/test13.log`，TEST_EXIT=0，≈ 4.5 min）；`make -s asan`（ASan+UBSan+LSan 常开 + TSan 5 组，含新加的 `tsan_g13 mode3`）
**ASAN_EXIT=0**（第一遍 TSan 抓到坑 109——观测接口无锁读，修后第二遍全绿，§7）。本文件所有数字都标了 **[实测]** 或 **[预估]**，实测的给命令。

---

## 0. 一页结论

| 项 | 改前（EVAL12 基线） | 改后（默认配置） | 代价 / 取舍 |
|---|---|---|---|
| **③ 读放大**：bench4 50k 纯插入后 run 数 / 区间扫描 vs SQLite | 26 条 / **3.9 ~ 4.3× 慢** | **10 条 / 1.0 ~ 1.6×**（绝对 7.8~8.3 → 3.5~4.8 µs/100 行）；点查 0.34 → 0.22× SQLite | 块层写 +30%（5.03 → 6.52 MB）；文件高水位 3.72 → 4.35 MB（活数据 2.86 MB） |
| ③ 同上 200k | 26 条 / 6.35× / 提交 max 50 ms | **10 条 / 2.3× / 提交 max 7.2 ms**；get 0.67 → 0.22× | **高水位 14.6 → 21.3 MB（活 9.0 MB，2.4×）**：合并产物要连续区间，洞装不下就长尾巴；块层写 18.0 vs 18.0 MB（持平） |
| ③ 写放大 bench3r（RamDev 随机覆盖） | 2.63× / 2.61× / 2.10× | 2.98× / 3.48× / 2.17×（三组：`20000 500` / `200000 2000` / `200000 2000 8`） | 读优先的代价；`set_merge_cheap_pages(0)` 或 `-DFCS_MERGE_CHEAP_DEFAULT=0` 回到老行为 |
| **④ 调用方提交延迟** wprof_lt 10 万键 × 500 B，150 × 2000 覆盖 | mode 1 不限预算：avg 30 / p90 108~116 / max 156~159 ms（>50 ms 36 次） | mode 1 预算 20 ms：avg 23.5 / **p90 29.7 / max 221~230**（>50 ms 3~5 次；欠账 4.3 s 在之后的提交里补）；**mode 3 线程：avg 4.7 / p90 6.1~6.5 / max 12 ms（>50 ms 0 次）** | mode 1 的 max 尾巴（220 ~ 570 ms）还在：不是单元粒度，是 L0 撞 4t 悬崖的兜底（§5）；mode 3 多一个线程、读者 p99 变差（下一行） |
| ④ 读者停顿 stall 10 万键 × 100 B 4 s | 单次 get max 13.8~17.8 ms；commit max 208~268 ms | mode 1：get max 9.0~11.2 ms，**commit max 26~31 ms**；mode 3：commit max 8.4~13.7 ms，get max 8.5~19.1 ms，p99 59 → 83~90 µs | mode 3 的维护线程与读者争同一把锁 |
| **② 归并 / 归一产物驻留内存** consol_rss 149 MB 库 `consolidate(0)` | 峰值 **+262 MB**（1.76 × 产物） | **+0 MB**（300000×500）；+5 MB（300000×100，0.2×）；+0 MB（60000×2500，236 MB 产物） | 预留 = 源页数 × 1.125 + 附件；产物超预留走搬家路径（g13 stream 段故意逼出 27 次） |
| **① seq 回绕** | 用尽前 256 号返回 FULL，到顶没有运维手段 | 提交点自动重编号（无快照 / 无作业 / 无未落盘范围墓碑时），`seqwrap 50 400` 回绕 400 次全对、重开全对 | 重编号那次提交写一份全量清单（run 数 × 40 B） |
| **⑤ 顶层 / 不满 t 条的脏 run** | 81% 脏的 2 条顶层 run 永不合并，空间放大 1.2~1.5× | (d)(e) 规则：最老前缀 / 单条 ≥ 50% 脏就并 / 重写，**归并现在会丢被组外更新盖住的记录**（否则白写） | 每重写 1 字节至少回收 1 字节（写放大上界 +1×） |
| 挂载 1M × 50 B 惰性账 | 30 ~ 39 ms | 18.5 ~ 19.4 ms [实测 ×3]（run 数 20 不变；补算 586~626 ms 分摊） | — |

顺手抓到并修掉 **6 个坑（104 ~ 109）**，其中坑 104 是**掉电丢已提交数据**（§3），改前的头就有。

---

## 1. 改了什么（机制）

### ① 提交点重编号（坑 103 彻底修法）
`commit_locked` 里 `next_seq_ ≥ seq_renum_at_`（默认 0x80000000，`set_seq_renumber_at` 可调，测试用）且无快照、无作业、无未落盘范围墓碑时：
`renumber_seqs_locked()` 把活 run 按老 seq 升序重编为 0x40000000 起的连续号，`SegMap::rekey` 重建每个单元的段键，写一份全量清单（`write_man_snapshot`）再写 SB。
挂钩无条件 `abandon_job()`；`renumber_runs()` 去重。观测：`seq_headroom()`、`seq_renumbers()`；用尽前仍返回 `FULL`（`Store::SEQ_EXHAUSTED`）。
坑：有快照时不能重编号（老视图按 seq 判胜负）⇒ 长期持快照的部署要看 `seq_headroom()`。

### ② 产物流式落盘
`RunWriter` 加流式字段；`begin_stream` 按源页数上界预留连续区间（`np + np/FCS_STREAM_SLACK_DIV(8) + 附件 + 4`，拿不到退 `np+2`，再拿不到就老路径），
`stream_spill` 每封满 `STREAM_CHUNK=256` 页就落盘（同步 `write_multi` / 异步入队），`finish_run` 预留分支把多余尾部 `release_reserve` 立即还回空闲表；
产物超出预留 ⇒ 停止流式、剩余页留内存、结束时整条搬家（`stream_moves`）。分步作业（MergeJob）的预留跨提交点活着，`abandon_job()` 归还。
整条 run 退休 / 归并源 run 不再逐段写 `M_SEG_DEL`（这是驻留内存的真因之一：149 MB 库 RSS 恒 44~49 MB）。
数据页 `+4` 的 next 链不入 CRC（寻址一律 page0+i）。异步模式归还预留前 `wait_pages`。

### ③⑤ 层合并规则（`level_pass`）
(a) 满 t 条且垃圾 ≥ 30%（原规则）；(b) ≥ 4t 条兜底（原规则）；
**(c) 满 t 条且 Σ页 ≤ `cheap_pages_`（默认 1024 页 = 4 MB）就并**（读放大触发，不等垃圾）；
**(d) 任意层最老的连续 1~t 条，垃圾 ≥ 50% 且 ≥ 64 页（单条：lvl ≥ 1 且 ≥ 256 页）⇒ 并 / 重写**，前缀长度取「回收 − 重写」最大者；
(e) 顶层单条 ≥ 256 页 ≥ 50% 脏 ⇒ 原地重写（是 (d) 单条在顶层的特例）。单条重写同样走分片作业。
**归并丢组外被盖记录**：`merge_run_set` / `merge_job_step` 的回调里 `newer_may_contain`（布隆）命中才 `newer_has(k, maxseq, …)` 精确确认，确认了就跳过（`shadow_dropped()` 观测）。
没有这一条 (d)(e) 只会白写（g13 (e) 段改前 422 页重写后还是 422 页、每次维护都再写一遍）。
**范围删除现在也算脏**：`settle_ranges` 按「段字节 × 活条数占比」估计墓碑前后的活字节差，从 run 的 `live_bytes` 扣（坑 106）。

### ④ 维护模式 / 预算
`set_maint_mode(m)`：0 提交前（老）；1 提交后由调用线程（默认），**每次最多 `FCS_MAINT_BUDGET_US`=20 ms 墙钟**（`set_maint_budget_us`，0 = 不限），
剩下的留给下一次提交（`maint_pending()`），L0 积压 ≥ 4t 时不受预算限制；2 不自动，调 `maintain()`；**3 后台线程** `mthr_`（按单元争锁，`~Store` 先 join，`open()` 自动停 / 重启，设备必须比 Store 活得久）。
`MaintStats` 加 `deferred / stream_runs / stream_pages / stream_moves`。

### 其它
`verify` 认 owner 8 = 作业的流式预留；`shrink()` 先 `abandon_job()`；`open()` 清 `pend_delta_`；`reload()` 把 `tail_` 抬到 pend_delta_ run 之上；`write_sb` 只在 `pend_delta_` 为空时开坑 99 的闸；shrink 的活区算上 `pending_free_`。

---

## 2. 验收（回归）

`tests/g13.cpp` 7 段，`make test` 的第 44 段，asan 列表与 TSan（`tsan_g13 mode3`）都加了：

| 段 | 验什么 | 判据（实测值） |
|---|---|---|
| stream | 流式产物 + 故意估小预留逼搬家 + 异步 | 流式 run 62、落页 8925、搬家 27；每步 verify + 全量对拍 |
| nospace | 预留拿不到（RamDev 2600 页）退老路径 / NOSPACE 不丢数据 | 成功 40 / 失败 0，重开一致 |
| renumber | `set_seq_renumber_at(0x40000100)` 逼重编号：快照期不重编、作业被放弃、范围墓碑等落盘 | 重编后 run 号连续、视图 / verify / 重开全对 |
| cut | 重编号 + 掉电（VolDev）：老 SB / 新 SB 两种落点 | 老 6 / 新 8 次，都等于某个提交点 |
| merge | (c) 纯插入 run 峰值 cheap=1024 → 14 vs 0 → 32；(d) A,B 全死 + C 活 → 只合 A+B、剩余垃圾 0%；(e) 顶层 422 → 106 页且不再重写；**(e2) lvl 1 单条 843 页被范围删除盖 60% → 337 页 lvl 2（15 片）**；(f) lvl_t=2 级联 | 全部 ok |
| budget | 1 µs 预算 × 48 次提交：推迟 41 次、run 峰值 33（兜底 164）、放开后欠账清零、verify 全对 | ok |
| mode3 | 后台线程 + 4 读者 + 提交线程，结束时 run 7 条、单元 898、峰值 12；`open()` 换设备时线程自动停 / 起 | ok |

其它改动的回归：g3 `t_delloop` 改为 `set_merge_cheap_pages(0)`（它验的是「整条被盖住的 run 当次退休」，cheap 规则把 10 条 3000 键的 run 并成 1+2 条后「删一半」盖不住任何一整条）。

---

## 3. 本轮抓到的坑（都已修，编号接 EVAL12 的 103）

| # | 现象 | 根因 | 修法 | 抓到它的 |
|---|---|---|---|---|
| **104**（**掉电丢已提交数据**，改前的头就有） | g6 seed 2：掉电重开后「既不等于上一提交点也不等于本步」，已提交的键 NOTFOUND | 提交点之外的维护（mode 1 提交后 / `maintain()` / mode 3）把源 run 的 DEL 留在 `pend_delta_`、页留在 `pending_free_`（坑 99 的闸关着），**盘上的 SB 还引用这些 run**；`shrink()` 只看 `live_top()`（内存里已没有这些 run）就截断，再写一个不含 DEL 的 SB（shrink 不是提交点，g5 明确依赖这一点）⇒ 掉电挂到的提交点引用设备之外的页。trace：`FREE [104] safe → SHRINK 105→104 → MOUNT → REPLAY rid=104` | shrink 的活区算上 `pending_free_`；`write_sb` 在 `pend_delta_` 非空时**不**开坑 99 的闸（那种 SB 不是提交点，下一次 `promote_pending` 不能放行） | g6（掉电注入）；第 12 轮也有这条路径（M2 的 `maintain()`），4 个种子没撞上 |
| 105 | q1 async2：同一个 `Store` 对象 `open()` 重开后 verify `owner beyond tail`，`compact_file` 后 400 行剩 240 | `clear_state()` 故意保留 `pend_delta_`（同会话 `reload()` 要用），但 `open()` 是新挂载：上一会话没过提交点的合并产物（分配在 SB 高水位之上）被重新挂上、源被摘掉、高水位按 SB 回退 ⇒ 产物页被当空闲页分出去 | `open()` 清 `pend_delta_`/`gpend_`；`reload()` 若 `pend_delta_` 非空把 `tail_` 抬到这些 run 之上（同会话 IO 错误重建的路径） | q1（第 12 轮没撞上是因为 20 条单页 run 不合并，cheap 规则让它合了） |
| 106 | g13 (e2)：lvl 1 单条被 `del_range` 盖 60% 后脏度仍 0 | run 级 `live_bytes` 由 flush 的 `probe_prev` / 归并 / 压实维护，**范围墓碑从不扣它**（只结单元级 live/dead）⇒ (a)(d)(e) 对范围删除全盲，只有单元压实按预算慢慢啃 | `settle_ranges` 用 `seg_totals`（memo，至多读两张边界页）估计段的活字节 × 活条数占比，墓碑前后差额从 `live_bytes` 扣；惰性账（未知单元 / `lb_unknown`）不碰 | g13 |
| 107 | g13 (d)(e)：A、B 各 60% 死（被 C 盖住）合并后产物一样大；顶层重写 422 → 422 页且每次维护都重写 | 归并只丢**组内**被盖副本 / 可丢墓碑 / 范围墓碑盖住的记录，不知道组外更新的 run 已经有该键 | 回调里布隆命中才 `newer_has` 精确确认，确认了跳过（`shadow_dropped_`）；若 probe 对墓碑返回非 OK 则不丢（安全不回收） | g13、`/tmp/dbg_d.cpp` |
| 108 | g13 budget：verify `page unowned` | `FCS_MERGE_BUDGET_DEFAULT=256` ⇒ mode 1 默认就会开分步作业；作业 `RunWriter` 的流式预留跨提交点，既不在清单也不在 `free_`；`shrink()` 截掉预留后作业写到设备之外 | verify 认 owner 8（作业预留）；`shrink()` 开头 `abandon_job()` | g13（g11 没撞上：源 < 512 页不预留） |
| 109 | 收尾 asan：TSan `tsan_g13 mode3` 1 条 data race（间歇，CPU 争用下必现） | `run_count()` 等 5 个结构型观测接口从不拿锁；mode 3 维护线程 `runs_.erase` 与主线程 `runs_.size()` 撞 | 这 5 个接口拿 `RGuard`（已持独占 ⇒ 空操作） | tsan_g13 mode3 ×12（争用下） |

设计教训写进 DESIGN3 §12：**「只写 SB」不等于「提交点」**——任何把 `pending_free_` 放行 / 把高水位往下挪的动作，前提都是「盘上 SB 不再引用这些页」，
而这只有折叠了 `pend_delta_` 的 SB 才成立。

---

## 4. 实测数字（本机沙箱 2 核，单次抖动 ±30%，都给中位或区间）

### 4.1 bench4 vs SQLite（FileDev，`WAL + synchronous=FULL`）[实测 ×3 / 200k ×1]
```
g++ -std=c++17 -O2 -pthread -Iinclude -DFCS_MERGE_CHEAP_DEFAULT=0 bench/bench4.cpp -o build/bench4_c0 -lsqlite3
g++ -std=c++17 -O2 -pthread -Iinclude bench/bench4.cpp -o build/bench4_c1024 -lsqlite3     # = 默认
build/bench4_c{0,1024} 50000 500 32 50000 2000 100      ×3
build/bench4_c{0,1024} 200000 2000 32 50000 2000 100    ×1
```
| 配置 | insert（FCS/SQLite） | update | get | range 100 行 | runs | 高水位 / 活 | 块层写 FCS / SQLite |
|---|---|---|---|---|---|---|---|
| 50k cheap=0（老行为） | 1.00 ~ 1.21× | 0.16 ~ 0.29× | 0.29 ~ 0.35× | **3.9 ~ 4.3× 慢**（7.8~8.3 µs） | 26 | 3.72 / 3.09 MB | 5.03 / 4.02 MB |
| 50k cheap=1024（默认） | 0.90 ~ 1.11× | 0.19 ~ 0.32× | 0.17 ~ 0.28× | **1.03 ~ 1.61×**（3.5~4.8 µs；SQLite 2.2~4.7） | 10 | 4.35 / 2.86 MB | 6.52 / 4.02 MB |
| 200k cheap=0 | 1.28× | 0.16× | 0.67× | 6.35×（14.7 µs） | 26 | 14.6 / 12.0 MB | 18.0 / 18.6 MB |
| 200k cheap=1024 | **0.61×** | 0.15× | **0.22×** | **2.29×**（4.9 µs） | 10 | **21.3 / 9.0 MB** | 18.0 / 18.6 MB |

200k 的提交 max：cheap=0 50.1 ms，cheap=1024 **7.2 ms**（L0 不再攒 26 条才并）。
高水位 2.4× 活数据的根因：合并产物要**连续**区间，L1→L2 的 1280 页产物装不进 8 个 160 页的洞 ⇒ 长在尾巴上；`shrink()`/`compact_file()` 能收回；
预留余量不是原因（`-DFCS_STREAM_SLACK_DIV={0,32}` 实测高水位 21.28 / 21.29 MB 不变）。

### 4.2 写放大 bench3r（RamDev，随机键覆盖）[实测 ×1，字节数确定]
| 负载 | cheap=0 | cheap=1024 |
|---|---|---|
| `20000 500` | 2.63× | 2.98× |
| `200000 2000` | 2.61× | 3.48× |
| `200000 2000 8`（lvl_t 8 显式） | 2.10× | 2.17× |

### 4.3 提交延迟 wprof_lt（FileDev，10 万键 × 500 B，150 × 2000 覆盖）[实测 ×2]
`build/wprof_lt 100000 500 150 2000 8 0 <mode> <budget_us>`（第 7/8 参新加；结束时 drain `maint_pending()` 并打印欠账）
| 模式 | avg | p50 | p90 | max | >50 ms | 写放大 | 备注 |
|---|---|---|---|---|---|---|---|
| mode 1 预算 0（= 第 12 轮） | 29.1 ~ 30.1 | 6.3 | 108 ~ 116 | 156 ~ 159 | 36 | 1.95× | 欠账 0 |
| **mode 1 预算 20 ms（默认）** | 23.4 ~ 23.5 | 25.5 ~ 25.9 | **28.5 ~ 29.7** | 221 ~ 230 | 3 ~ 5 | 1.83 ~ 1.86× | 推迟 138 ~ 143 次，欠账 4.2 ~ 4.3 s 在结束时补；run 25 ~ 28 条 |
| **mode 3 线程** | **4.7** | 4.6 ~ 4.8 | **6.1 ~ 6.5** | **11.9 ~ 12.2** | **0** | 1.95× | run 31 条 |

### 4.4 读者停顿 stall（FileDev，10 万键 × 100 B，写者每批 2000 put + commit，1 读者 64 get 一批，4 s）[实测 ×2]
`build/stall 100000 100 4 0 0 <mode>`
| 模式 | commit max | 单次 get max | >1 ms / >10 ms | p50 / p99 |
|---|---|---|---|---|
| mode 1（默认预算） | 26.1 ~ 30.6 ms | 9.0 ~ 11.2 ms | 345~348 / 0~1 | 6.3 ~ 6.5 / 59 ~ 61 µs |
| mode 3 | 8.4 ~ 13.7 ms | 8.5 ~ 19.1 ms | 500~530 / 0~2 | 6.1 ~ 6.2 / 83 ~ 90 µs |
（EVAL12 基线 mode 1 不限预算：commit max 208 ~ 268 ms、get max 13.8 ~ 17.8 ms。）

### 4.5 内存 consol_rss [实测 ×1]
`build/consol_rss <n> <vb> <per>`：`300000 500 10000` 写后 RSS 44 MB，`consolidate(0)` 峰值 **+0 MB**（改前 +262）；`300000 100 10000` +5 MB（改前 +17）；`60000 2500 2000` +0 MB（改前 +36）。

### 4.6 其它
`build/seqwrap 50 400`：RESULT OK（回绕 400 次、重开 OK）。`build/mount 1000000 50 50000 3 1`：open 18.5 ~ 19.4 ms（基线 30 ~ 39）。
`build/g13 merge`：(d) 垃圾 0%、(e) 422 → 106 页、(e2) 843 → 337 页。

---

## 5. 明说的坑与未做

1. **文件高水位**（cheap=1024 + 连续产物）：200k 纯插入 2.4× 活数据（cheap=0 1.22×）。根治要么产物分段（格式）、要么合并前先 `relocate`。现在的手段：定期 `shrink()` / `compact_file()`，或 `set_merge_cheap_pages(0)`。
2. **mode 1 预算的尾巴（已定位）**：p90 压到 30 ms，但 max 221 ~ 300 ms 还在。不是单个单元慢（`-DFCS_PROF_MAINT` 实测没有单元 > 12.5 ms；
   mode 2 手工按 20 ms 循环 `maintain(1)` 最坏提交 22.7 ms、最坏单元 24.6 ms），而是**预算内的维护吞吐 < 写入速率**：每次提交进 250 页 L0，
   20 ms 只够跑 1 ~ 2 片（256 源页）⇒ L0 停在 31 条（= 4·lvl_t 悬崖下一格），每撞到 32 条就触发"积压不受预算限制"的兜底，一口气并掉
   ⇒ 78 ~ 122 ms 的周期性尖峰和一次 567 ms（`/tmp/wp_err.cpp`：`commit 146: L0 31->34 567.7 ms`）。预算只能挪工作不能减工作；
   mode 3 之所以 max 12 ms 是第二个核在干活。
   **为什么 20 ms 不够**（每次提交只进 250 页 = 1/8 个 L0 组，一片 256 页 ≈ 10 ms，按理一片就追得上）：分步作业收尾时把触及的
   **全部单元标成"账未知"**（R12 的设计：片与片之间账可能被改，增量公式不成立 ⇒ 交给补算），wprof 负载下一次作业收尾 = 5000 个单元
   未知（`unk 0→5146`），补算每单元 64 个、每个 3 ~ 8 ms ⇒ 一次作业收尾背上 300 ~ 600 ms 的补算，比归并本身还贵，预算全花在补算上。
   **试过**"预算随 L0 积压分级放大（≥ 2t ×2、≥ 3t ×3）"：max 220 / 526 ms、欠账 4.5 s 一点没变，stall 读者反而更差 ⇒ 不进引擎。
   下一步（§8）：作业收尾改成和一次性归并一样的单元级增量账（`CellAcc` 快路径已经有了，只差"片与片之间被改过的单元"的判定）。
3. **mode 3 读者 p99 变差**（59 → 83 ~ 90 µs）：维护线程与读者争同一把锁，锁粒度是单元。
4. **写放大 +13 ~ 33%**（bench3r）是读优先的直接代价，默认接受；写多读少的部署关掉 cheap 规则。
5. 范围删除的脏度是**估计**（段字节 × 活条数占比），只影响合并时机不影响正确性；估多了最多多并一次（产物的账精确）。
6. (d) 单条重写只对 lvl ≥ 1 且 ≥ 256 页的 run：L0 小 run 等组合并顺手丢垃圾更便宜；小库（< 1 MB 的 run）的范围删除垃圾仍靠单元压实。
7. 重编号需要「无快照」：长期持快照的部署 `seq_headroom()` 会一路下降到 FULL；没有强制手段（强制 = 快照视图失效）。
8. P2 格式升版（seq/ver 64 位、稀疏 run、B2）仍单开，不混入。

---

## 6. 复现命令与时长

| 命令 | 时长 |
|---|---|
| `make -j2 -s test`（44 段） | ≈ 4.5 min（沙箱 2 核） |
| `make -s asan`（ASan+UBSan+LSan + TSan 5 组） | ≈ 25 ~ 30 min |
| `build/g13 {stream,nospace,renumber,cut,merge,budget,mode3}` | 全部 ≈ 8 s |
| `bash /tmp/run_bench.sh`（bench4 ×8 + bench3r ×6，脚本内容见 §4.1/4.2） | ≈ 3 min |
| `build/wprof_lt 100000 500 150 2000 8 0 {1 -1, 1 0, 3}` | 每次 ≈ 8 ~ 10 s（含欠账） |
| `build/stall 100000 100 4 0 0 {1,3}` | 每次 4 s + 装载 |
| `build/consol_rss 300000 500 10000` | ≈ 30 s |
| `build/seqwrap 50 400`、`build/mount 1000000 50 50000 3 1` | ≈ 10 s / 25 s |
| 调试用：`G6_LOG=1 build/g6 2 2>log`、`G5_LOG=1 build/g5 1`、`FCS_TRACE` 编译 + `stdbuf -o0 -e0` | — |

## 7. asan / TSan

第一遍 `make -s asan`：ASan+UBSan+LSan 全部 PASS，**TSan `tsan_g13 mode3` 报 1 条 data race**（间歇：裸跑 6 次都不复现，
并行跑 `build/g6` 制造 CPU 争用后第 1 次就抓到）：主线程 `run_count()` 读 `runs_.size()`，维护线程在 `merge_job_step` 里 `runs_.erase`。
根因：结构型观测接口（`run_count / lvl_hist / cell_count / seg_max / max_segs_per_cell`）从来不拿锁，mode 3 之前这是对的（只有调用线程改账），
mode 3 之后不对。修：这 5 个接口拿 `RGuard`（共享锁；本线程已持独占 ⇒ 空操作，可重入）。修后同样的争用下 12 次 0 报告。
第二遍（`backup/r13/asan13.log`）：见下行（跑完后填）。

- 第二遍 `make -s asan`（最终头 `backup/r13/fcs.h.final`，03:06 ~ 03:27，21 min）：**`ASAN_EXIT=0`**，ASan / UBSan / LSan / TSan 全部 PASS（`backup/r13/asan13.log`）。
- 同一个头的第六次 `make -j2 -s test`：44 段 `TEST_EXIT=0`（`backup/r13/test13.log`，5.5 min）。

## 8. 下一轮的优先级（按本轮数据）

1. **作业收尾的增量账**（替代"全部单元标未知"）：这是 mode 1 预算失效、L0 停在悬崖边、周期性 100 ~ 570 ms 尖峰的根因；
   做完预算才真正有效（预估 p90 不变、max 从 220 ~ 570 降到 ≈ 一片的时间 + 一次 fsync，欠账清零）。
2. **文件高水位**：合并前先把源 run 旁边的洞合并（relocate 小 run）或允许产物分段（格式）；现在 200k 纯插入 2.4× 活数据。
3. mode 3 读者 p99（59 → 85 µs）：单元内部再切（归并片 ≤ 64 页）或读者优先锁。
4. P2 格式升版单开（seq / ver 64 位、稀疏 run、产物分段一起上）。
