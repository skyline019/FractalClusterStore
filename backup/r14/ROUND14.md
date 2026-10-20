# 第 14 轮执行与收尾（2026-10-20）

范围 = `docs/20-rounds/ROUND13.md` §8 的前三项，**三项全做**：(1) 分步作业收尾改单元级增量账；(2) 文件高水位；(3) mode 3 读者 p99。
不含 P2 格式升版（仍单开一轮；§5 明说本轮哪些事只有格式升版才能做）。
改前头 `backup/r14/fcs.h.pre-r14`，项 1+3 定稿中间态 `backup/r14/fcs.h.item13`，改后头 = `include/fcs.h`（收尾后复制为 `backup/r14/fcs.h.final`）。

**收尾状态**：`make -j2 -s test` **45 段全绿**（5 min 13 s）、`make -s asan` **`ASAN_EXIT=0`**（26 min 48 s），TSan 循环 16 次 0 报告，详见 §7。所有数字都标 **[实测]** 或 **[预估]**，实测的给命令；沙箱 2 核，单次抖动 ±30%。

---

## 0. 一页结论

| 项 | 改前（ROUND13 基线） | 改后（默认配置） | 代价 / 取舍 |
|---|---|---|---|
| **(1) mode 1 预算真正有效** wprof_lt 10 万键 × 500 B，150 × 2000 覆盖，预算 20 ms | avg 28~31 / p90 32 / **max 382~549 ms**（>50 ms 7~10 次）；欠账 4.7~5.2 s | avg 15~16 / p50 9 / p90 31~33 / **max 37~44 ms**（>50 ms **0**）；**欠账 0** | 写放大 1.72~1.83× → 1.94×（作业收尾不再把 touched 单元全标未知 ⇒ 补算扫描 2.1~2.5 s → 32~46 ms，但也不再顺手把这些单元压实一遍：垃圾留给规则 (d)） |
| (1) 同上 mode 3 | avg 5.4~5.9 / p90 6.6~8.1 / max 13.8~17 | avg 5.9 / p90 7.4 / max 13.5 | — |
| **(3) 读者 p99** stall 10 万键 × 100 B，写者定节拍 50 ms（§4.3 解释为什么必须定节拍） | mode 1：p99 36 µs / 单次 get max 22.6 ms / >10 ms 2；mode 3：**p99 60 µs** / max 9.0 ms / >1 ms 412 | mode 1：**p99 20 µs** / max 5.7 ms / >10 ms 0；mode 3：**p99 17.6 µs** / max 5.6~11 ms / >1 ms 222；读者吞吐 **×2.1~2.3** | 维护单元按 1 ms 时间片让路（`set_maint_quantum_us`，0 = 关），unit_max 4.2 ms；剩余 5~6 ms 地板 = 提交路径两次 fsync 在排他锁内（§5） |
| **(2) 文件高水位**：合并时刻峰值 layout 探针 200k × 500 B 插入 + 10 万覆盖 | 峰值 **2.95×** 活数据（预留按源页数上界 2726 页，产物 1728）| 峰值 **2.60×**（预留按存活字节估 ⇒ 1900 页量级）；800k：峰值 1.88× → 1.56×，末态 1.74× → 1.53× | 估小了先**就地续租**（预留下方留 32~256 页"清单页簇/小 run"洞，续租不被堵），续不动才搬家；g14 实测续租 2 次 0 搬家 |
| (2) 合并后静态占用（同上末态） | **2.43×**（bench4 200k B=2000：hw 21.3 MB / 活 9.0 MB = 2.4×） | 默认 **2.48×**（bench4 22.0 MB，2.44×）—— 顶层合并的产物只能贴尾、源洞在它下面，**现格式下不可避**（§1.2）；开 `set_hw_reclaim_pct(150)`：layout B=500 **1.40×**、B=2000 **1.16×**、bench4 **1.97×**（17.6 MB） | 回收 = 把顶部 run 重写进低洞：块层写 +16~24%（layout）/ +22%（bench4）；默认关 |
| (2) 洞被切碎 | 相邻 L0 run 之间夹 1 页清单页 ⇒ 退休后 8 个 269 页的洞而不是 1 个 2152 页的洞 | 清单页一次长 8 页、7 页留作"清单页簇"，run 分配第一遍跳过簇 ⇒ 洞能连成片（g14 t_hw：默认 1.58× → 回收后 1.03×） | 高水位 +1~2%（簇 + 续租洞）|

本轮没有新坑编号（没有改坏既有语义的 bug）；抓到的"假象"和设计性发现在 §3。

---

## 1. 改了什么（机制）

### (1) 分步作业收尾：单元级增量账（`merge_job_step` 收尾段，`Cell::jacc/jdirty`，`MergeJob::JAcc/Late`）

改前：作业落地时 `mark_unknown(touched)`，把作业碰过的所有单元标未知，之后每个维护单元用预算补算 —— 补算一遍 4000 单元 ≈ 2.1~2.5 s，mode 1 的 20 ms 预算只是把这 2 s 切成 100 多个提交尾巴（max 382~549 ms 的真因）。

改后：每片归并时按单元累计 `JAcc{em（产物里落给该单元的条数）, sup（被组外更新盖住、丢掉的条数）, vis_old（该单元里源 run 段的旧存活字节）, sup_new（新段里被盖住的字节）}`；
片后新写覆盖同键 ⇒ `flush_mem` 的 `probe_prev` 钩子 `job_overwrite` 把差额记到 `Late`；期间该单元被压实 / 切分 / 范围墓碑碰到 ⇒ `jdirty`。
收尾：`jacc && !jdirty && acct` ⇒ 公式直接更新 `live/dead/seg.live`（`++job_fast_`），否则老路 `mark_unknown`（`++job_slow_`）。
FCS_TRACE 构建里收尾后再 `recount` 对账：g11 47 作业 / g13 94 / wprof 17 全部 0 mismatch；g14 `acct_drift` 段在**非 trace 构建**里对所有已知单元做同样的对账（0 漂移）。

放弃的方案（别再试）：收尾集中 probe（和老路一样贵）、近似 osz、分级预算（无效已回退）。墓碑被覆盖分不清"搬 / 丢"（坑 94）⇒ 整单元 jdirty 回退补算。

### (2) 文件高水位（四件事 + 一个结论）

**1.2.0 先说结论（layout 探针，§4.4）**：合并产物要连续区间；源 run 的页要到**下一个提交点**才进 `free_`；产物 ≈ Σ 源 ⇒ 产物永远装不进"现有的洞"，顶层合并时 hw ≈ 活数据 + 产物 ≈ 2×。
这和 LSM 全量 compaction 的峰值一样，**现格式下不可避**；"合并前 relocate 小 run 把洞并起来"对此无效（总空闲 < 产物：layout 末态空闲 600 页 vs 产物 1728 页）。真正降峰要 P2 的 `M_RUN_TRIM`（源 run 前缀随产物分片逐步释放）或顶层按键区分片（§5）。
本轮做的四件事都在这个结论之内：

1. **预留按存活字节估**（`stream_cap_est`）：`Σ ceil(live_bytes/页载荷) × 1.125 + meta + 4`，上限仍是老的 Σ源页 × 1.125。脏组（规则 (a)/(d) 挑的就是 ≥ 30%/50% 脏）按页数预留会把文件临时撑大一倍。`live_bytes` 只会高估（flush 的 probe_prev 逐键扣、范围墓碑按比例扣），估小是少数。
2. **估小 ⇒ 就地续租**（`stream_spill` → `extend_reserve`）：预留区贴着尾部或后面是洞时零拷贝续租；续不动才退回老的搬家路径（`stream_moves`）。为了让续租不被堵，预留落在尾部时在它**下面**留一个 `cap/16`（32~256 页）的洞：作业跨提交点，期间的清单页 / 小 L0 run 没洞可用就会贴着预留区上方长尾（g14 t_extend 第一版：第一个提交就把 8 页清单压在预留区上面，续租失败搬家 1 次）。
3. **顶部空闲段释放 ⇒ `tail_` 回落**（`promote_pending` 末尾）：以前 `tail_` 只在 `shrink()` 回退，顶部 run 退休 / 被搬走之后 hw 仍按旧高水位报、增长也从旧 `tail_` 起。只收紧分配器，不截文件（截文件仍是 `shrink()` / `compact_file()` 的事）。清单页簇贴着尾部时不收（收了下一条 run 又贴着清单页长）；判定是"≤ 8 页且起点在 `man_hole_`"——簇上方的 run 退休后 `coalesce` 把簇并进大洞，起点标记还在但段已不是簇，这种照收（收尾全量回归前补的一行，`g14 recede` 覆盖簇不收、`layout` 覆盖并洞后回落）。
4. **清单页簇**（`man_hole_`，`alloc_man_page_raw` 4)）：清单页长尾时一次长 8 页、7 页留给后续清单页；`alloc_pages` 第一遍跳过 ≤ 8 页的簇洞、也不拿它和尾部拼块。否则每个提交点的 1 页清单夹在相邻 run 之间，run 退休后洞被切成"块 − 1 页"的小段，下一代产物装不进只能长尾。尽力而为、不持久（`rebuild_free` 清空）。

**可选策略 `set_hw_reclaim_pct(pct)`（默认 0 = 关）**：维护空闲时（无作业、层合并无事）若 `tail_ > live_page_count() × pct/100`，把**文件最顶上的 run** 搬进更低的洞（同 ver 同 seq，和 `relocate()` 一条路；块 > `merge_budget_` 走单源分步作业、预留 = 块 + 4，否则当场重写），源块下个提交点释放 ⇒ 顶部空出 ⇒ `tail_` 回落；回到阈值内就停。每搬一条顶部严格下降 ⇒ 必然终止。为什么搬最顶上的而不是最大的：产物上面常压着刚 flush 的 L0 run，它们可能要等 4t 条才合并（规则 (b)），不搬走它们尾部永远回不来，而搬一条 155 页的 L0 只花 155 页写。快照钉住的 run 不搬（P5）。
它只治"合并后静态占用"，治不了合并时刻的峰值（搬家发生在峰值之后）；代价是被搬的 run 多写一遍（§4.4：+16~24% 块层写）。所以默认关，空间敏感的部署自己开。

**试过无效（已回退）**：`alloc_pages` 改最佳适配（能装下的最小洞）—— layout 200k 末态 4678 → 4598 页（−2%）、800k 不变；大洞根本不是被小分配切碎的，是产物从来装不进。保留"最低地址先用"（尾部紧凑、`shrink` 友好）。

### (3) mode 3 读者让路（`XLock`、`maint_unit`、切片 `cut`、`recount_sweep`）

- `XLock`：读者先 200 次 `cpu_relax()` 自旋、再 64 次 `yield`、再睡；`rwant_` 计数 ⇒ `contended()`。
- 维护单元时间片 `set_maint_quantum_us(us)`（默认 `FCS_MAINT_QUANTUM_US` 1000；0 = 关）：`maint_yield_now()` = 有读者在等 **且** 本单元已跑满时间片。切片归并在**单元格边界** `cut`（并把 `j.acc` 里 `lo >= cut` 的登记 pop 掉——否则下一片重复累计，账就错了）；`recount_sweep` 每个单元后检查；`maint_loop` 单元之间最多 64 次 `yield` 让读者插队；mode 3 提交线程的背压 `maint_loop(budget, true)` 守预算（hard）。
- 观测：`MaintStats.yields / unit_max_us / unit_long（>4 ms 单元数）`。
- **第一版（别再试）"任何人一等就让"**：读者常驻等待 ⇒ 每格切片 ⇒ 维护吞吐垮 ⇒ mode 3 背压无限 ⇒ commit max 869 ms、get p50 51 µs。必须有最小时间片，背压须守预算。

### 其它

- `MaintStats` 新增字段：`sweep_us rebal_us level_us recounts job_fast job_slow yields unit_max_us unit_long stream_extends hw_reclaims`。
- `begin_stream(w, src, cap_hint)` / `start_job(set, newlvl, cap_hint)`：调用方可给预留页数（单源搬家用）。
- bench4 第 16 参 `hwpct`；`# fcs space:` 行多打 `largest_free / extents / hw_reclaims / pending`，并在量空间前把欠的维护干完 + 一个空提交（让最后一批释放页过提交点）。
- 新探针 `bench/probe/layout.cpp`（§4.4）；`stall` 第 8 参 `pace_ms`、多打 `maint:` 行；`wprof_lt` 多打 `maint:` / `lvl_hist:` 行。

---

## 2. 验收（回归）

`tests/g14.cpp`（Makefile 第 45 段；asan 循环含 g14；TSan 新增 `tsan_g14 yield`）：

| 段 | 判据 | 实测输出（-O2） |
|---|---|---|
| `jobacct` | 120 次提交（覆盖 + 删除 + 范围删除，作业切 32 页小片跨很多提交点）：`job_fast > 0`；欠账清零；`unk_cells_ == 0`；**所有已知单元 live/dead/段 live 与 recount 一致**；verify + 真值 | job_fast=14212 job_slow=0，提交后有欠账 0 次，提交 max 7.2 ms |
| `reserve` | 2400 页 run 80% 脏，规则 (d) 单条重写：峰值增长 ≤ 产物 × 1.4 + 64；走流式；0 搬家 | 产物 480 页，峰值增长 603 页（1.26×；老估法 ≈ 5.62×） |
| `extend` | 把源 `live_bytes` 压到 1/4 再重写：`stream_extends > 0` 且 `stream_moves == 0` | extends=2 moves=0 |
| `hw` | 同一负载（8 × 5000 插入 ⇒ L1；覆盖 60% ⇒ (d) 重写；再 10 批小写）跑三遍：pct=0 不回收；pct=150 mode 1 / mode 3 都回收且 tail 比默认低 ≥ 15%；账零漂移 | 默认 1.58×（写 19.0 MB）→ pct=150 **1.03×**（回收 2 次，写 24.6 MB），mode 3 同 |
| `recede` | 白盒：尾部分配 300 页 → 释放（pending）→ SB 之前不回落、提交点之后 `tail_` 退回起点；清单页簇在顶不回落；回落后继续写 + 重开一致 | ok |
| `yield` | mode 3 + 读者线程，70 次提交：quantum 1 ms ⇒ `yields > 0`（≥ 2 核时断言）；quantum 0 ⇒ `yields == 0`；读无错；账零漂移 | pass 0：单元 225 / 让路 37 / unit_max 2.9 ms；pass 1：让路 0 |

`build/g14` 单跑 ≈ 8 s；`build/g14 <jobacct|reserve|extend|hw|recede|yield>` 单段。

TSan（`-O1 -fsanitize=thread`）：`tsan_g13 mode3` × 8 + `tsan_g14 yield` × 8 交替跑（`/tmp/tsan_loop14.sh`）：**16 次 0 报告** [实测]。

---

## 3. 本轮抓到的"假象"与设计性发现

1. **项 3 第二版"p99 变差"是假象**：不定节拍时写者因为不再等长单元，4 s 内提交 82~92 → 184~200 次，读者撞到更多提交期 flush，p99 / >1 ms 看似翻倍。给 stall 加第 8 参 `pace_ms` 让写者定节拍（每次提交至少间隔 50 ms）后 A/B 全面改善（§4.3）。**以后比读者停顿必须定写者节拍。**
2. **读者停顿地板 ≈ 5~6 ms**：提交路径 `flush_mem → wait_io → commit_delta → write_sb` 两次 `dev_->sync()` 全在排他锁内。把 fsync 移出排他锁 = "可见先于持久"的语义变更（另一个读者可能读到还没落盘的提交），本轮不做（§5）。
3. **切片 `cut` 后必须 pop `j.acc` 中 `lo >= cut` 的登记**：否则项 1 的增量账在让路点重复累计（g14 `yield` 段的账零漂移就是盯这个）。
4. **就地续租会被清单页堵死**：作业跨提交点，提交的清单页 / 小 L0 run 没洞就贴着预留区上方长尾。修法是预留下方留洞（1.2 之 2）。
5. **相邻 run 之间的 1 页清单页把洞切碎**（1.2 之 4）。这是 ROUND13 "高水位 21.3 MB" 里一部分碎片的来源，但不是主因（主因是 1.2.0）。
6. **`tail_` 回落只在最后一批释放页过了提交点之后**：bench4 / layout 量空间前要再提交一次，否则量到的是 pending 状态（bench4 已改）。
7. **mode 1 的维护进度不确定**（墙钟预算）：同一负载两次的 hw 末态可差几百页。比较高水位用 layout 探针多跑几次或看峰值。

---

## 4. 实测数字（本机沙箱 2 核，单次抖动 ±30%，都给命令）

### 4.1 提交延迟 wprof_lt（FileDev，10 万键 × 500 B，150 × 2000 覆盖）[实测 ×2~3，最终头 ×1]

```
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/wprof_lt.cpp -o build/wprof_lt
build/wprof_lt 100000 500 150 2000 8 0 1 -1      # mode 1 预算 20 ms
build/wprof_lt 100000 500 150 2000 8 0 1 0       # mode 1 不限预算
build/wprof_lt 100000 500 150 2000 8 0 3         # mode 3
```
| 配置 | avg | p50 | p90 | max | >50 ms | 欠账 | WA |
|---|---|---|---|---|---|---|---|
| mode 1 20 ms **改前** | 28.2~31.4 | 27.4 | 32.4~32.7 | **382~549** | 7~10 | 4.7~5.2 s | 1.72~1.83× |
| mode 1 20 ms 改后 | 15.2~16.4 | 8.8~10 | 31~33 | **36.7~43.7** | 0 | **0** | 1.94× |
| mode 1 不限 改后 | 14.4 | 8.0 | 37.8 | 56 | 1 | 0 | 1.95× |
| mode 3 改后 | 5.4~5.9 | 5.6~6.0 | 6.6~8.1 | 13.5~17 | 0 | 0 | 1.94× |

维护分解（前 → 后）：sweep 2143~2459 → 32~46 ms；rebal 742~1031 → 365~417；level 745~847 → 860~988 ms；recounts 87k → 8~9k；job_fast ≈ 132k，job_slow 0~1.1k。verify=OK。
WA 1.8 → 1.94×：改前作业收尾把 touched 单元全标未知，补算时顺手压实了这些单元（垃圾早回收），改后垃圾交给规则 (d)；p90 不变、max 降 10 倍换 +6% 写。

### 4.2 读者停顿 stall（FileDev，10 万键 × 100 B，写者每批 2000 put + commit **且提交间隔 ≥ 50 ms**，1 读者 64 get 一批，4 s）[实测 ×2]

```
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/stall.cpp -o build/stall
cp backup/r14/fcs.h.pre-r14 /tmp/oldinc/fcs.h; cp include/blockdev.h /tmp/oldinc/
g++ -std=c++17 -O2 -pthread -I/tmp/oldinc -Iinclude bench/probe/stall.cpp -o build/stall_old
build/stall_old 100000 100 4 0 0 {1,3} -1 50 ; build/stall 100000 100 4 0 0 {1,3} -1 50
```
| | 旧 mode 1 | **新 mode 1** | 旧 mode 3 | **新 mode 3** |
|---|---|---|---|---|
| reader 批次（吞吐） | 8196~9291 | **18044~18793** | 9186~9319 | **19823~21541** |
| p50 / p99 / p99.9 µs | 1.85~6.6 / 22~36 / 394~403 | 1.42~1.58 / **13.8~20.3** / 120~124 | 1.87~1.99 / **44~60** / 538~555 | 1.34~1.37 / **15.1~17.6** / 112 |
| single get max | 14.6~22.6 ms | **4.9~6.4 ms** | 9.0~16.8 ms | 5.6~11.3 ms |
| >1 ms / >10 ms | 310~363 / 2 | 188~220 / 0 | 412~434 / 0~2 | 194~222 / 0~1 |
| commit max | 27~32 ms | 25.7~26.8 ms | 7.4~9.5 ms | 4.5~5.1 ms |
| maint units / yields / unit_max / >4 ms | — | 801~804 / 74~108 / 4.1~4.2 ms / 1~2 | — | 842~867 / 156~199 / 3.7~4.3 ms / 0~3 |

不定节拍（无第 8 参）的数字只能和自己比：新头 mode 1 p99 88~98 µs / max 6.0~6.6 ms / 134~141 commits，mode 3 p99 134~145 / max 5.8~6.3（基线 68~69 / 13~17 ms / 75~77 commits；70~80 / 9.7~12.8）—— 差异来自写者提交翻倍（§3.1）。

### 4.3 为什么 A/B 必须定节拍

stall 的写者是 "put 2000 条 → commit → 立刻下一批"。改前写者每次提交后要等维护单元（最长 20 ms 一片），改后单元 1 ms 就让路 ⇒ 写者提交速率翻倍 ⇒ 读者撞到两倍的"提交期排他锁 + fsync"。p99 看的是读者，但负载变了。`pace_ms` 把提交间隔钉住后两边负载相同。

### 4.4 文件高水位 layout 探针（RamDev → `build/layout.db`，bench4 同款负载：插入 N 每 B 条提交，再随机覆盖 N/2）[实测 ×1~2]

```
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout
g++ -std=c++17 -O2 -pthread -DLAYOUT_OLD -I/tmp/oldinc bench/probe/layout.cpp -o build/layout_old
build/layout{,_old} N B 32 1 1 [compact=0|1(compact_file)|2(relocate+shrink)] [hw_pct]
```
| 负载 | 旧头末态 / 峰值 | 新头默认 末态 / 峰值 | 新头 pct=150 末态（写量） |
|---|---|---|---|
| 200k B=500（+10 万覆盖） | **2.43× / 2.95×**（4678 / 5674 页，活 1922） | 2.48× / **2.60×**（4765 / 5001） | **1.40×**（2664 页；写 41.3 → 47.9 MB，+16%，回收 2 次） |
| 200k B=2000 | 2.35× / 2.41× | 2.35× / 2.41× | **1.16×**（2207 页；写 29.9 → 37.0 MB，+24%，回收 4 次） |
| 800k B=500（+40 万覆盖） | 1.74× / 1.88× | **1.53× / 1.56×**（写 169.0 → 169.1 MB） | 1.53×（回收 1 次就没有装得下顶部 run 的洞了） |
| 纯插入阶段（覆盖前） | 200k 1.33×、800k 1.16× | 同 | 同 |

末态布局（200k B=500，旧头）：`[洞138][L1 40][L1 41][L1 41][洞431][L2 289][洞1966][L3 1730]` —— 顶层产物 1730 贴尾，它的源（Σ 2312 页）留下 1966 页的洞在它下面。
手动命令（旧头末态上）：`compact_file()` → 1.10×，多写 28.8 MB（归一 + 搬家 + 检查点 + 收缩）；`relocate() + shrink()` → 1.45×，多写 6.8 MB。

bench4 200k（`build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 <hwpct>`，FileDev）[实测 ×1]：默认 hw **22.0 MB** / 活 9.0 MB（ROUND13 21.3）；pct=150 **17.6 MB**（1.97×；最大洞 893 页装不下顶部的大 run，回收 2 次就停），块层写 168 → 206 MB（+22%）。`file=` 36.4 MB 是峰值时扩的文件长度（只有 `shrink()` 截文件）。

### 4.5 写放大 bench3r [预估]

本轮没有改合并规则；预留 / 清单页簇 / 回落都不改写量（layout 800k 写量 169.0 → 169.1 MB）。回收策略默认关。bench3r 的三组数字应与 ROUND13 持平（2.98× / 3.48× / 2.17×）—— 收尾 `make test` 里的 WA 段过了即视为持平，没有单独重跑 bench3r。

---

## 5. 明说的坑与未做

1. **合并峰值 2×（现格式）**：见 1.2.0。真正的修法是 P2：(a) `M_RUN_TRIM(run, new_page0)`：产物每落一片就把源 run 已归并完的前缀释放 ⇒ 峰值 ≈ 活数据 + 一片；重放时 run 的段表要按 trim 后的起点重建（`attach_run` 按 fence 建段 ⇒ 被 trim 的单元必须有 `M_SEG_DEL`，或者 trim 记录本身带键界）。(b) 顶层按键区分片（leveled 顶层）：产物按稳定键界切成多条 run，下一代只和同键区的 run 合并 ⇒ 每次合并源集只有一个分区；需要稳定的分区边界（跨代一致，否则源被部分消费又回到 trim 问题）。两者都是格式变更，单开一轮。
2. **读者停顿地板 5~6 ms**：提交期两次 fsync 在排他锁内。移出去 = 可见先于持久（提交返回前另一读者能读到未落盘数据；掉电后它读过的东西消失）。要做需要"提交分两段：锁内切换内存视图 + 锁外 fsync + 读者视图版本门"，属语义变更，下一轮单独讨论。
3. **回收策略默认关**：它用 +16~24% 的块层写换静态占用，对 WA 敏感的部署不合适；而且只治静态占用不治峰值（设备仍要有 2× 余量过顶层合并）。
4. **就地续租仍可能失败**：预留在洞里（不在尾部）且洞后面不是空闲 ⇒ 走搬家（`stream_moves`，正确但多写一遍）。估小本身少见（`live_bytes` 只会高估，除范围墓碑的比例扣）。
5. **mode 1 WA 1.8 → 1.94×**（4.1）：接受——p90 不变、max 降 10 倍。
6. **best-fit 回退**、**项 3 第一版回退**、**分级预算回退**：§1。
7. `tail_` 回落不截文件：`file=` 仍是峰值长度；要还给文件系统照旧 `shrink()` / `compact_file()`。

---

## 6. 复现命令与时长

| 命令 | 时长 |
|---|---|
| `make -j2 -s test`（45 段） | ≈ 5~6 min（沙箱 2 核） |
| `make -s asan`（ASan+UBSan+LSan + TSan 6 组，含新加的 `tsan_g14 yield`） | ≈ 25~35 min |
| `build/g14 {jobacct,reserve,extend,hw,recede,yield}` | 全部 ≈ 8 s |
| `build/wprof_lt 100000 500 150 2000 8 0 {1 -1, 1 0, 3}` | 每次 ≈ 3~10 s |
| `build/stall{,_old} 100000 100 4 0 0 {1,3} -1 50` | 每次 ≈ 9 s |
| `build/layout 200000 500 32 1 1 [0|1|2] [pct]`、`build/layout 800000 ...` | ≈ 10 s / 16 s |
| `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 {0,150}` | ≈ 3~13 s |
| FCS_TRACE 对账：`g++ -std=c++17 -O1 -g -pthread -Iinclude -DFCS_TRACE tests/g13.cpp -o build/trace_g13; build/trace_g13 2>/tmp/t.log; grep -c "JOB MISMATCH\|JOB CHECK\|R1 MISMATCH" /tmp/t.log` | ≈ 21 s |
| TSan：`/tmp/tsan_loop14.sh`（g13 mode3 × 8 + g14 yield × 8） | ≈ 3 min |

## 7. test / asan（最终头 `backup/r14/fcs.h.final`，7234 行）

| 命令 | 结果 | 时长 | 日志 |
|---|---|---|---|
| `make -j2 -s test` | **45 段全绿**（`TEST_EXIT=0`；第 13 轮 44 段 + g14 6 段） | 5 min 13 s（2 核；收尾前一次 4 min 55 s） | `backup/r14/test14.log` |
| `make -s asan` | **`ASAN_EXIT=0`**：ASan + UBSan 全部 PASS（g13 35.2 s、g14 16.3 s），TSan `w1 / g7 ×3 / g11 / g13 mode3 / g14 yield` 0 报告，无 LeakSanitizer | 26 min 48 s | `backup/r14/asan14.log` |
| `tsan_g13 mode3` + `tsan_g14 yield` 循环 8 轮（`/tmp/tsan_loop14.sh`） | `runs=16 bad=0` | ≈ 4 min | 沙箱 /tmp（不持久） |
| FCS_TRACE 对账（`-DFCS_TRACE` 编 g13 / g14，`grep -c "JOB MISMATCH\|JOB CHECK\|R1 MISMATCH"`） | g13 全段 **0 mismatch**（90 条 JOB 账对上）；g14 其它 5 段 0 mismatch | ≈ 2.5 min | — |

**trace 构建下 `g14 jobacct` 的 60 s drain 不够**（FAIL "欠账清不掉"，账本身 0 mismatch）：`-DFCS_TRACE` 每个维护单元都带整单元 recount，一个单元就超 20 ms 预算 ⇒ 每次提交只做一个单元，60 s / 510 次提交清不完 ≈ 2000 个单元；`-O2`（0.5 s、drain 0 次）与 ASan（2.0 s、drain 0 次）都正常。这是 trace 构建的成本问题，不是引擎问题；不改测试。

坑：收尾全量回归跑完后又补了 §1.2 第 3 条那一行（簇并进大洞后照收），所以 test / asan 都是补完之后**重跑**的数字；第一次 test 的日志被覆盖。

## 8. 下一轮的优先级（按本轮数据）

1. **P2 格式升版单开**：seq / ver 64 位、`M_RUN_TRIM`（或顶层分区）、稀疏 run —— 合并峰值从 2× 降到 "活 + 一片" 只有这条路。
2. **提交路径 fsync 出锁**（语义讨论先行）：读者停顿地板 5~6 ms → 亚毫秒。
3. 回收策略的二步搬家（先把挡路的小 run 搬开再搬大 run）：bench4 200k 卡在 1.97× 就是"最大洞 893 页 < 顶部大 run"。
