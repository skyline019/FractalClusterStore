# 第 10 次讨论用的探针（不是引擎的一部分，Makefile 不编译它们）

全部只读引擎；`instrument.patch` 是给 `include/fcs.h` **临时**加计时器的补丁（讨论轮不改引擎，
补丁只用于 `/tmp/inc_m` 副本）：

    rm -rf /tmp/inc_m && cp -r include /tmp/inc_m && (cd /tmp/inc_m && patch -p0 fcs.h < /home/user/embdb/x/bench/probe/instrument.patch)

| 文件 | 量什么 | 命令 |
|---|---|---|
| `fs.cpp` | 沙箱 fdatasync 地板 | `g++ -O2 bench/probe/fs.cpp -o /tmp/fs && /tmp/fs` |
| `cprof.cpp` | 同步提交 CPU vs fsync 拆分（RamDev / FileDev / commit(false)） | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/cprof.cpp -o /tmp/cprof && /tmp/cprof 1000 10` |
| `mprof.cpp` | consolidate / compact_file 的独占持锁时长随库大小 | `/tmp/mprof 200000 2000 100 3`、`/tmp/mprof 1000000 5000 100 2` |
| `stall.cpp` | 写者满负荷/空闲时读者 get 延迟分布（共享读） | `/tmp/stall 100000 100 4 {-1,0,20}`（-1 写者空闲，0 无维护，20 每 20 次提交 consolidate） |
| `wprof.cpp` | 覆盖型负载逐提交耗时分布；`FCS_PROF=1` 看阶段 | `/tmp/wprof <keys> <vb> <commits> <per>` |
| `wprof2.cpp` | （需补丁）level 后 recount 占比 | `g++ … -I/tmp/inc_m bench/probe/wprof2.cpp`；`/tmp/wprof2 100000 100 150 2000` |
| `wprof3.cpp` | （需补丁）rebalance 拆分：split / compact / compact_cell 内部 / 层级直方图 | `/tmp/wprof3 100000 500 145 2000 145`、`/tmp/wprof3 20000 500 200 500 50` |

`prototype.patch`（在 `instrument.patch` 之上）是讨论里 R0 的**估算原型**：compact_cell 一趟数引用 + 压实输出层号夹到 LVL_MAX +
顶层参与 level_pass。只用来回答「修了能回到哪」，没有进工作区引擎；正式实现见 `docs/DISCUSS10.md` R0 的方案（引用计数字段 + 合批输出）。

    cp /tmp/inc_m/fcs.h /tmp/inc_m/fcs.h.instr && (cd /tmp/inc_m && patch -p0 fcs.h < /home/user/embdb/x/bench/probe/prototype.patch)
    g++ -std=c++17 -O2 -pthread -I/tmp/inc_m bench/probe/wprof3.cpp -o /tmp/wprof3p && /tmp/wprof3p 20000 500 200 500 50

---

# 第 11 次讨论追加（只读轮：剩余优化点 + 老正确性缺陷取证）

引擎仍然一字未动。`dbg_hooks.patch` 给 `include/fcs.h` 的**副本**加调试接口（`dbg_key` / `dbg_ranges` / `dbg_live_check`
+ `commit()` 各阶段边界的账目自检），用法：

    rm -rf /tmp/inc_dbg2 && cp -r include /tmp/inc_dbg2 && (cd /tmp/inc_dbg2 && patch -p0 fcs.h < /home/user/embdb/x/bench/probe/dbg_hooks.patch)

| 文件 | 量什么 / 复现什么 | 命令 |
|---|---|---|
| `storm.cpp` | 规模化工况 × 每提交逐键全量比对（含快照视图）× 可选每提交 verify；flags 见文件头 | `g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/storm.cpp -o /tmp/storm && /tmp/storm rsb 1 3000 120 200` |
| 同上（调试头） | 首个坏键处 dump 单元 / 逐提交监视某键 | `g++ … -DSTORM_DBG -I/tmp/inc_dbg2 … -o /tmp/storm_dbg && STORM_WATCH=2859 STORM_WATCH_EVERY=1 /tmp/storm_dbg r 1 3000 120 200` |
| `tomb_snap.cpp` | **坑 93**：`add_tomb` 支配规则删旧墓碑无视快照 ⇒ 快照视图复活被删键（退出码 1 = 复现） | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/tomb_snap.cpp -o /tmp/tomb_snap && /tmp/tomb_snap` |
| `tomb_snap_rb.cpp` | 坑 93 补充：同场景 `rollback` 不受影响 | 同上 |
| `tomb_leak.cpp` | **坑 92**：悬空点墓碑 ⇒ `exact_live_recs` 校准出幻影存活 ⇒ 全死 run 不退休 + verify `run live recs` | 同上 |
| `tomb_forever.cpp` | **坑 94**：点墓碑永不清除，写一次/删一次负载线性增长 | 同上 |
| `tomb_fast.cpp` | **坑 95**：`settle_ranges` 整盖快路径 vs 归并口径（点墓碑）⇒ 快照钉住时 verify `live mismatch`；`NOSNAP=1` 对照 | 同上 |

gprof：`g++ -std=c++17 -O2 -g -pg -pthread -Iinclude bench/probe/wprof.cpp -o /tmp/prof/wprof_pg && (cd /tmp/prof && mkdir -p build && ./wprof_pg 100000 500 150 2000 && gprof -b -p ./wprof_pg gmon.out | head -40)`。

---

# 第 11 轮执行追加

四个坑都已修进引擎，上面的 4 个 `tomb_*.cpp` 探针和 `storm` 现在应当全部 OK（它们的「老头 FAIL / 新头 PASS」版本固化在 `tests/g9.cpp`）。

| 文件 / 脚本 | 用途 | 命令 |
|---|---|---|
| `wprof.cpp` 第 5 个参数 | `set_level_merges(n)`（R2 预算，默认 0） | `build/wprof 100000 500 150 2000 1` |
| `storm.cpp` | `STORM_MAX_FAILS`（默认 20，对比老头时设大）；`STORM_RUN=<n>` 分步维护算子；`l` 标志 = 预算 1 | `STORM_MAX_FAILS=100000 timeout 300 /tmp/storm rsb 1 3000 120 200` |
| `../ab.sh` | 两份头文件 **交替**编译/运行 N 轮取中位（H4）；两份头都必须存在，否则直接退出（第一次踩过：cp 失败 ⇒ A == B） | `bench/ab.sh backup/r11/fcs.h.pre-r11 include/fcs.h bench/probe/wprof.cpp 3 -- 100000 500 150 2000` |
| gprof | 同一探针加 `-pg`，看 `merge_emit<recount>` 调用数（R1 前 82k / 后 11k） | `g++ -std=c++17 -O2 -g -pg -pthread -Iinclude bench/probe/wprof.cpp -o /tmp/wp && /tmp/wp 100000 500 150 2000 && gprof -b -p /tmp/wp gmon.out \| head -20` |

性能基线：`backup/r11/fcs.h.pre-r11`（第 11 轮动手前的头；和步 0 之后的头只差坑 92/93/95 的修法，对 wprof 负载性能无影响）；
各阶段快照 `backup/r11/fcs.h.post-{R1,94,R2,R8}`。

---

# 第 12 次讨论追加（只读轮：剩余优化点 + 坑 98 取证）

引擎未动。结论在 `docs/DISCUSS12.md`。

| 文件 / 脚本 | 量什么 | 命令 |
|---|---|---|
| `wprof_lt.cpp` | 同 wprof，第 5 参 = `lvl_t`（1000 = 关掉层合并 ⇒ flush 地板 1.13×），第 6 参 = level_merges；末尾打印设备写字节 / 写放大 / verify | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/wprof_lt.cpp -o build/wprof_lt && build/wprof_lt 100000 500 150 2000 {2,4,8,16}` |
| `mount.cpp` | 挂载时间（含 `recompute_liveness`）、首批 get、每次 open/close 的 RSS 增量（**坑 98**） | `build/mount 1000000 50 50000 8`；泄漏取证：`g++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread -Iinclude bench/probe/mount.cpp -o build/mount_asan && build/mount_asan 200000 50 50000 8` |
| `storm.cpp` 新标志 | `T` = `TrashDev`：discard 过的页立刻填 0xDD + `set_discard_min_pages(1)`；`A` = `set_range_tomb_auto(6)` | `build/storm rsbocalT 1 3000 120 200` |
| `storm_matrix.sh` | 12 工况 × {T, A, TA} × 4 种子（≈ 7 min，144 组） | `bench/probe/storm_matrix.sh` |
| `FCS_PROF=1` | `commit()` 五个阶段逐次打印（flush_mem / rebalance / level / commit_delta(=wait_io+清单+write_sb) / promote） | `FCS_PROF=1 build/wprof 100000 500 150 2000 \| grep prof` |

---

# 第 12 轮执行追加（引擎改了：L1 / L2 / M2 / C1 / C2 / M1；数字在 `docs/DISCUSS12.md` §6）

性能基线：`backup/r12/fcs.h.pre-r12`（第 12 轮动手前的头）；中间版本 `backup/r12/fcs.h.{l1l2-v1,m2-v1,m2-v2,m2-v3}`。

| 文件 / 脚本 / 开关 | 量什么 | 命令 |
|---|---|---|
| `stall.cpp` 新输出 + 第 5 参 | 多打一行 **单次 get** 的最长等待 / >1 ms / >10 ms 次数（分片后 64 次一批的批时长看不出效果）；第 5 参 = `set_maint_units_max`（改前头编译加 `-DSTALL_OLDAPI`） | `build/stall 100000 100 4 0 [units_max]` |
| `mount.cpp` 第 5 参 | 1 = 惰性存活账（默认）/ 0 = 老行为（open 时 `recompute_liveness`）；多打印未知单元数和 mode 2 下补算到零的单元数 / 时长（改前头编译加 `-DOLDAPI`） | `build/mount 1000000 50 50000 3 {1,0}` |
| `wprof_m.cpp` | wprof + 维护旋钮：第 6 参维护模式、第 7 参 units_max、第 8 参切片预算；结束时**排空**维护再报 run 数与层分布（mode 1/2 的 run 数要排空后比才公平）。`wprof.cpp` 本体保持老 API 以便 `ab.sh` 对比改前头 | `build/wprof_m 100000 500 150 2000 0 {0,1} {0,8,32} [budget]` |
| `wprof_lt.cpp` | 同前；T1 实验用 `{8,4}` 两档（自适应旋钮已删） | `build/wprof_lt 100000 500 150 2000 {8,4}` |
| `../bench3.cpp` 第 4 参 | `lvl_t`（T1 的小记录负载一侧） | `build/bench3r 200000 2000 0 {8,4}` |
| `rep.cpp` | L2 开发期的模式 × 预算小矩阵（RamDev，90 次提交，每次全量比对）；正式回归在 `tests/g11.cpp` | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/rep.cpp -o build/rep && build/rep {4,32,0} {0,1} 90` |
| `-DFCS_PROF_MAINT` | 编译期开关：每个 >10 ms 的维护单元在 stderr 打印分解（sweep / rebalance+retire / level，含未知单元数、作业片数、run 数变化） | `g++ -std=c++17 -O2 -g -pthread -Iinclude -DFCS_PROF_MAINT bench/probe/stall.cpp -o /tmp/prof/stall_p && /tmp/prof/stall_p 100000 100 4 0 2>/tmp/maint.log` |
| `-DFCS_MERGE_BUDGET_DEFAULT=<n>` / `-DFCS_MAINT_MODE_DEFAULT=<m>` | 编译期改默认（A/B 不切片 / 老模式时用，不用改探针） | `g++ … -DFCS_MERGE_BUDGET_DEFAULT=0 bench/probe/stall.cpp -o /tmp/prof/stall_B0` |
| `-Dprivate=public` | 看私有状态的一次性探针（例：逐条 run 的 lvl / seq / live_bytes / 脏度）；不要进工作区 | 见 DISCUSS12 §6.3 的 run 表 |

坑：`bench/ab.sh` 要 `bash bench/ab.sh …`（文件没有执行位）；stall / mount 的计时在后台跑 `make test` 时不可信（2 核），先量再回归。

| `verswrap.cpp` | **坑 102** 边界探针：16 位逻辑提交版本回绕。`verswrap <commits> <phys_every> [snap_at]`；`snap_at>0` 在该次提交后持有快照跨回绕 | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/verswrap.cpp -o build/verswrap && build/verswrap 70000 256 65000`（老头 `-I/tmp/inc102` 编：RESULT: BROKEN） |
| `seqwrap.cpp` | **坑 103** 边界探针：run 序号 u32 回绕。`#define private public` 预置 `next_seq_`，`seqwrap <headroom> <commits>`；加闸前 RESULT: BROKEN（2493/3000 旧值），加闸后 `commit()` 返回 FULL、数据全对 | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/seqwrap.cpp -o build/seqwrap && build/seqwrap 50 400` |
| `consol_rss.cpp` | 归并 / 归一产物驻留内存的刻度：写 n × vb，`consolidate(0)`，打印前后 RSS / VmHWM（149 MB 库 ⇒ 额外 262 MB） | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/consol_rss.cpp -o build/consol_rss && build/consol_rss 300000 500 10000` |
| `peakrss.cpp` | 沙箱没有 `/usr/bin/time`：fork/exec 并每 20 ms 采样子进程 VmRSS，结束打印峰值 RSS / 墙钟 / 退出码 | `build/peakrss build/bench6 rget 2 100000` |
| `../eval.sh` | 全量评估矩阵（amp / sqlite / bigscan / mt / rget / snapmaint / gc / ovw / wprof / stall / mount / storm / robust / meta），每条命令原样入日志 | `OUT=/tmp/eval.log REPS=3 bash bench/eval.sh [段...]`（全部 ≈ 20 min） |

**坑（沙箱）**：`/tmp` 是 992 MB 的 tmpfs，算在 2 GB 内存里——探针把 `.db` 写到 `/tmp`（一个 249 MB 的 wprobe.db + 223 MB 的
/tmp/probe）叠上两个 g++ 就把机器拖进换页（两个 12 s 的编译跑了 19 分钟）。数据库文件一律放 `build/`（磁盘），日志才放 `/tmp`。

### 第 13 轮新增 / 改参（2026-10-20，数字在 `docs/20-rounds/ROUND13.md`）

| 文件 / 开关 | 做什么 | 怎么跑 |
|---|---|---|
| `wprof_lt.cpp` 第 7 / 8 参 | 第 7 参 = 维护模式（1 提交后由调用线程 / 3 后台线程），第 8 参 = mode 1 墙钟预算 µs（-1 引擎默认 20000，0 不限）；结束前 drain `maint_pending()`，打印 mode / budget / wall（+欠账）/ deferred | `build/wprof_lt 100000 500 150 2000 8 0 {1 -1, 1 0, 3}` |
| `stall.cpp` 第 6 / 7 参 | 第 6 参 = 维护模式，第 7 参 = 预算 µs；结束后补欠账并打印 runs / deferred | `build/stall 100000 100 4 0 0 {1,3}` |
| `consol_rss.cpp` | 不变；现在量的是流式产物（峰值 +0 MB） | `build/consol_rss 300000 500 10000` |
| `seqwrap.cpp` | 不变；现在验的是自动重编号（RESULT OK） | `build/seqwrap 50 400` |
| `-DFCS_MERGE_CHEAP_DEFAULT=<页>` | (c) 规则阈值（0 = 关 = 老行为）；bench4 / bench3r 的 c0 / c1024 变体就是它 | `g++ -std=c++17 -O2 -pthread -Iinclude -DFCS_MERGE_CHEAP_DEFAULT=0 bench/bench4.cpp -o build/bench4_c0 -lsqlite3` |
| `-DFCS_MAINT_BUDGET_US=<µs>` / `-DFCS_STREAM_SLACK_DIV=<n>` | mode 1 预算默认；流式预留余量 = 源页数 / n（0 = 无余量；实测对高水位无影响） | 同上 |
| `tests/g13.cpp` 单段 | `build/g13 {stream,nospace,renumber,cut,merge,budget,mode3}`（编译需 `-Wno-unused-function`，Makefile 已带） | ≈ 8 s 全部 |

坑：`wprof_lt` mode 1 的 max（220 ~ 570 ms）不是单元粒度（单元全 ≤ 12.5 ms），是 L0 撞 4·lvl_t 悬崖触发"积压不限预算"的兜底（ROUND13 §5）；p90 才是预算的效果；mode 3 的 wall 含后台线程抢 CPU（2 核沙箱）。

### 第 17 轮新增 / 改参（2026-10-20，数字在 `docs/20-rounds/ROUND17.md`）

| 文件 / 开关 | 做什么 | 怎么跑 |
|---|---|---|
| `fsync_block.cpp`（新，只用公开 API，新老头都能编） | ① 的直接证据：`HookDev::do_sync` 睡 D ms，写者线程 `commit()`，主线程在设备睡在 sync 里时做一次 `get`，打印 get 延迟。老头（锁内 sync→SB→sync）≈ 2D ms；新头 ≈ µs | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/fsync_block.cpp -o build/fsync_block && build/fsync_block 40 5`；老头 `-Ibuild/r16 -Iinclude`（先 `cp backup/r17/fcs.h.pre-r17 build/r16/fcs.h`） |
| `famw.cpp`（新，`#define private public`） | ③ 的微基准：小片（`set_merge_part_pages(part)`）× 小 `lvl_t` 让 L2→L3 归并的源 = t 个家族 × 很多片。打印耗时、runs/层直方图、峰值、`level_us`、dev 读写、verify；新头多一行 `merge width: src_max= cur_max=`。**老头在源 > 64 时 verify CORRUPT（live mismatch）**——这是 ③ 顺带修掉的真 bug，不是探针问题 | `build/famw [N=400000] [B=500] [part=48] [lvl_t=4] [budget=64]`；`famw 400000 500 16 8 64` ≈ 7 s（源 151 / 游标 8） |
| `layout.cpp` 末尾多一行 `== merge width: src_max= cur_max= … hole_fits=`（`#ifdef FCS_R17`） | 一次归并最多的源 run 数 / 游标数、按洞分片触发次数 | 参数不变 |
| `bench/bench4.cpp` 末尾多一行 `# fcs r17: merge src_max= cur_max= hole_fits= durable=`（`#ifdef FCS_R17`） | 同上 + `durable_vers()` | 参数不变 |
| `-DFCS_PART_HOLE_FIT=0` / `set_part_hole_fit(false)` | 关掉"中等产物按洞分片"（②(c3)），拿老行为对照 | `g++ … -DFCS_PART_HOLE_FIT=0 bench/probe/layout.cpp -o build/layout_nohf` |
| `-DFCS_LVL_CLIFF=<n>` / `-DFCS_LVL_DIRTY_PCT=<p>` / `-DFCS_LVL_DIRTY_BIG_PCT=<p>` | ② 的三个旋钮的编译期默认（运行时 `set_lvl_cliff / set_lvl_dirty_pct / set_lvl_dirty_big_pct`）；A/B 的变体就是它们 | `backup/r17/timing_ab.log` 里每个变体的编法在首行 |
| `hfprobe.cpp`（新，`#define private public`） | ②(c3) 调研：layout 200k 同款负载（RamDev），逐提交打印 tail 抬升 + 最大洞 + 层直方图、末态每层 run 页数、峰值、`hole_fits`；第 2 参维护预算 µs（0 = 无）、第 3 参开关、第 4 参 `set_maint_units_max` | `build/hfprobe 200000 0 1 8`（≈2 s；开 1.82× / 关 `… 0 8` 1.97×；`… 0 1 0` = 全挤一个提交点的 2.32×） |
| `tests/g17.cpp` 单段 | `build/g17 [iters]`（iters = 多写者轮数，默认 150；TSan 用 60） | ≈ 5.4 s 全部 |

坑：layout mode 1 有 20 ms 维护时间预算 ⇒ 布局不确定（同头 800k 峰值 1.40× 与 1.53× 都出现过），A/B 只信 > 0.1× 的差异、关键数字多跑取中位；`set_maint_budget_us(0)`（无预算）+ 默认 256 单元会把整个大归并挤进一个提交点、释放页全卡在 `pending_free_` ⇒ 峰值反而更高（200k 2.32×），要确定性布局用 `budget 0 + set_maint_units_max(8)`（g17 `t_hole_fit` 的做法）。

### 第 16 轮新增 / 改参（2026-10-20，数字在 `docs/20-rounds/ROUND16.md`）

| 文件 / 开关 | 做什么 | 怎么跑 |
|---|---|---|
| `layout.cpp`（参数不变：`layout N B VSZ mode quiet compact pct`） | 末行 `== after update:` 的 `peak`/`moves`/`runs` 是分片归并的三个观测量；quiet=0 的逐提交行里 `holes a/b pg max h` 看"洞够不够一片"。新头 vs 老头：`mkdir -p build/old && cp backup/r16/fcs.h.pre-r16 build/old/fcs.h`，老头编译加 `-Ibuild/old -Iinclude`（include 顺序决定命中哪份头） | `build/layout 200000 500 32 1 1 0 0`（2 s）；`build/layout 800000 500 32 1 1 0 0`（10 s）；2M ≈ 55 s |
| `-DFCS_TRACE` 的三行新轨迹 | `JOB choose_part best_hole=… -> part=… [lo,hi]`（按洞挑片）、`JOB reserve est=… part=… res_p0=… res_np=… tail=…`（推迟预留落在哪）、`JOB part -> seq=… pg=… srcs_left=…`（封口） | `g++ … -DFCS_TRACE bench/probe/layout.cpp -o build/layout_tr && stdbuf -o0 -e0 build/layout_tr 200000 500 32 1 0 0 0 2>&1 \| grep -a "JOB choose_part\|JOB reserve\|JOB part"`（≈90 s，trace 很多） |
| `-DFCS_PART_MIN_PAGES=<n>` / `-DFCS_PART_FLOOR_PAGES=<n>` | 片大小上限（默认 1024；也是"产物不到它就不分"的门槛）/ 下限（默认 64）。floor 32 的 layout 数字在 ROUND16 §3.1（峰值 −0.03~0.06×，run 数 +15~30%） | `g++ … -DFCS_PART_FLOOR_PAGES=32 bench/probe/layout.cpp -o build/layout_f32` |
| `seqwrap.cpp` | 改为 64 位：预置 `next_seq_ = 2^32 − headroom`，验"越过 2^32 不回绕、不重编号、数据全对" | `build/seqwrap 50 400` |
| `tests/g16.cpp` 单段 | `build/g16 {parts,reopen,rollback,snap,ovfl,tomb}` | 1.6 s 全部 |

坑：分片的收益对"合并开始前有多少脏源"不敏感但对源数 N 很敏感（起步长尾 ≈ N × 活比 × 1.1 片）；看峰值先看 quiet=0 轨迹里合并开始那一行的 `tail/live_top`，那部分不是分片能压的。

### 第 14 轮新增 / 改参（2026-10-20，数字在 `docs/20-rounds/ROUND14.md`）

| 文件 / 开关 | 做什么 | 怎么跑 |
|---|---|---|
| `hwmap.cpp`（**第 15 轮**，`#define private public`） | bench4 同款负载（N 插入 + N/2 覆盖，B 条/批），在 bench4 量 space 的那一刻打印 hw/活 比值、块层写、`hw_reclaims/hw_blockers`、run 地址布局与洞表；第 5 参 = 之后再做几个空提交（看策略还能不能推进，卡住就是二步搬家要解的）；第 6 参 = 维护模式 | `build/hwmap 200000 2000 32 150 60`（≈2 s；pct 矩阵 `for p in 0 110 130 150 170 200`） |
| `layout.cpp`（第 14 轮，`#define private public`；第 15 轮加 `blockers=` 与 `-DFCS_HW2_TRACE` 规划轨迹） | bench4 同款负载（插入 N 每 B 条提交，再随机覆盖 N/2），逐提交打印 `tail_` 抬升事件、末态 run 地址布局（含洞）、洞尺寸直方图、峰值 tail、流式预留 / 续租 / 搬家 / 回收次数、设备写量；DB 写 `build/layout.db` 自删。第 5 参 quiet=1 不打逐提交行；第 6 参 compact：1 = 末尾再调 `compact_file()`、2 = `relocate()+shrink()`（量代价与收益）；第 7 参 = `set_hw_reclaim_pct` | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout; build/layout 200000 500 32 1 1 [0|1|2] [pct]`（≈ 10 s；800k ≈ 16 s）。旧头对照：`-DLAYOUT_OLD -I/tmp/oldinc` |
| `stall.cpp` 第 8 参 `pace_ms` | 写者每次提交至少间隔这么多 ms（提交快了就 sleep 补足）。**比读者停顿必须定节拍**：不定节拍时改后的写者提交速率翻倍，读者撞到更多提交期 flush，p99 看似变差（ROUND14 §3.1）。新增 `maint:` 行（units / yields / unit_max / units>4ms / 分解 / job_fast / job_slow） | `build/stall 100000 100 4 0 0 {1,3} -1 50`；旧头 `cp backup/r14/fcs.h.pre-r14 /tmp/oldinc/fcs.h; cp include/blockdev.h /tmp/oldinc/; g++ ... -I/tmp/oldinc -Iinclude bench/probe/stall.cpp -o build/stall_old` |
| `wprof_lt.cpp` | 新增 `maint:` 行（units / slices / sweep / rebal / level 毫秒 / recounts / job_fast / job_slow / yields / unk_left）和 `lvl_hist:` 行 | 参数不变 |
| `bench/bench4.cpp` 第 16 参 `hwpct` | `set_hw_reclaim_pct`；`# fcs space:` 行多打 `largest_free / extents / hw_reclaims / pending`，量空间前先把欠的维护干完 + 一个空提交 | `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 150` |
| `-DFCS_MAINT_QUANTUM_US=<µs>` | 维护单元让路时间片默认（1000；0 = 不让路），运行时 `set_maint_quantum_us` | 同上 |
| `tests/g14.cpp` 单段 | `build/g14 {jobacct,reserve,extend,hw,recede,yield}` | ≈ 8 s 全部 |

坑：mode 1 的维护进度按墙钟预算走，同一负载两次的末态 hw 可差几百页，比高水位看峰值或多跑几次；`tail_` 只在最后一批释放页过了提交点之后才回落，量空间前再提交一次。
