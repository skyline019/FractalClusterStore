# XDB

嵌入式存储引擎。三代实现并存于本目录：

| 版本 | 文件 | 结构 | 写放大（20000/500） |
|---|---|---|---|
| v1 | `../src/engine.cpp` | 页内定位式 B+ 树 | 56.2× |
| v2 | `include/store.h` | B+ 树 + 事务/快照/崩溃自愈 | 22.5× |
| **v3（FCS）** | **`include/fcs.h`** | **cell / 段 / run 的分形视图 + 布隆门控 + 页内前缀压缩 + 零写回收 + 分层合并 + 异步组提交 + run 元数据页 + 溢出页大值 + 32 位页号** | **2.70×（大提交 2.35×）** |

文档分类索引：[`docs/README.md`](docs/README.md)。  
v3 设计见 **`docs/10-design/DESIGN3.md`**；评估主文 **`docs/00-entry/EVAL17.md`**；v2 设计见 `docs/10-design/DESIGN2.md`。

> 2.70× 比上一版（2.53×）高 0.17，全部是 **run 元数据页**的开销（≈3.25 B/记录）；
> 它换来的是挂载时间 `O(总页数) → O(run 数)`：400k 记录的库重开 **42 ms → 11 ms**。
> 这笔取舍在 DESIGN3 §7.1 / §10.6 里明写，宏 `FCS_RMETA_MIN_PAGES` 可调。

## v3 与 SQLite 正面对比（bench4，同负载同批次）

50000 键 / 32 B 值 / 每 500 次操作提交 / 区间扫 100 行 × 2000 次；
SQLite `WAL + synchronous=FULL`，FCS `FileDev` + 提交屏障。
数字为 3~5 次运行的中位区间（共享 CPU，单次抖动 ±30%）。

| 阶段 | FCS 同步组提交 | FCS 异步 + 整理 | SQLite 3.46 |
|---|---|---|---|
| 写入（含提交） | 0.63 µs/op | **0.51** | 2.1~2.2 |
| 覆盖写（含提交） | 1.14 | **0.83** | 10.8~18.2 |
| 点查 | 0.60 | **0.55** | 1.5~1.9 |
| 区间扫描（100 行） | 1.90 | **0.57** | 1.5~1.9 |
| 文件大小 | 2.90 MB | **2.10 MB** | 2.35 MB |
| 每记录字节 | 58.1 B | **41.9 B** | 46.9 B |

> 表内 FCS 两列是本机今日实测（`bench4 50000 500 32 50000 2000 100 0 1 1 8 0 1 1`），
> SQLite 同批运行；写放大与 `docs/10-design/DESIGN3.md` §10.2 一致。

> 三个常数项的清理把写入从 2.6~3.3 拉到 1 µs 量级：**切分后收紧页区间**（原来每个单元都
> 从 run 首页开始扫，O(单元数×页数)）、**CRC 走 SSE4.2**（374 MB/s → 7.5 GB/s）、
> **异步批量入队**。覆盖写快 SQLite 15~40 倍（FCS 的更新只追加一条 run，不碰旧页、不写 WAL）；
> 区间扫描靠"分散键域摘要 `sp` + 可选 `kidx` + 归一后只剩 1 条 run"压到 0.55 µs。
> 有序键再省 11% 字节（页内前缀压缩，41.9 → 37.4 B/条）。

## 查询层

```cpp
Store s;  s.open(&dev, nullptr, 512);

s.put(k, val, len);  s.del(k);  s.commit();     // 写（值最长 65534 B）
s.del_range(lo, hi);                            // 范围删除（清单墓碑，扫描零特判）
s.get(k, buf, cap, &len);                       // 点查（布隆门控，≈1 页）

Store::Cursor cur;                              // 可暂停 / 可重定位 / 可定时
cur.open(&s, lo, hi);
while (cur.next(k, v, len)) { ... }

s.select(lo, hi, fn, limit, offset);            // 区间扫描 + 分页
s.select_prefix(pfx, bits, fn);                 // 前缀扫描
s.select_at(vers, lo, hi, fn);                  // 快照视图（时间旅行）

s.index_put(idx, skey, pk);                     // 二级索引（与主记录同一次提交 ⇒ 原子）
s.index_scan(idx, slo, shi, fn);                // 索引区间扫描（回调拿到 skey, pk）
s.index_get(idx, skey, pk);

s.commit(false);  ...  s.commit();              // 组提交：N 次逻辑提交 → 1 次物理提交

// —— 部署整理 / 常驻整理态 ——
s.set_tidy_policy(Store::TidyPolicy::Soft);    // Soft=configure_deploy+护洞；Hard/Periodic 再配 maybe_tidy()
s.configure_deploy();                          // async + cheap + hole_fit + 护洞 + hw_reclaim 150
s.commit(false);  ...  s.commit();             // 组提交仍由应用拼
auto sn = s.tidy_snap();                       // runs / layout_pct / protect_holes …
while (s.maybe_tidy() == RC::MORE) {}          // Hard=consolidate_step；Periodic=compact_file_step
s.polish();                                    // 对外读服务前重锤：consolidate(0)+shrink
s.set_protect_merge_holes(true);               // 护洞：用户 flush 不吃 ≥floor 中部大洞（合并仍可进）
s.set_part_frag(true);                         // 养洞合并 A：洞碎时降片下限（默认开；part_frags() 观测）
s.set_part_frag_floor(16);                     // 碎片态最小片（页）
s.set_part_coalesce(true);                     // 养洞合并 B：预留前并洞（默认关；layout 抬峰，见 ROUND18 §6）

s.set_async_writes(true);                       // 异步：flush 放后台（configure_deploy 已开时可省略）
s.set_async_flush_min(65536);                   // 后台一次落多少条（默认 65536 或 4 MB 工作区）
s.set_io_threads(4);                           // 后台写线程数（设备队列深时压停顿）
s.set_lvl_t(16);                               // 分层旋钮：小=读优化，大=写优化（覆盖型大值负载建议 4，见 docs-hist/DISCUSS12 §6.6）
s.set_maint_mode(1);                           // 0 = 提交前维护；1 = 提交后（默认）；2 = 应用自调 maintain；3 = 后台维护线程
s.set_maint_units_max(256);                    // 一次 commit() 之后最多跑多少个维护单元
s.set_merge_budget_pages(256);                 // 层合并切片预算（页）；0 = 不切片
s.set_maint_mode(3);                           // 后台维护线程（commit 墙钟短；读者 p99 见 ROUND14）
s.set_maint_budget_us(20000);                  // mode 1 每次 commit 后最多维护 20 ms（0 = 不限）
s.set_maint_quantum_us(1000);                  // 维护单元让路时间片（µs）；0 = 不让路
s.set_hw_reclaim_pct(150);                     // 高水位回收（默认 0）；二步搬家见 g15 / ROUND15
s.set_merge_part_pages(0);                     // P2b：0 = 自适应按洞挑片（ROUND16）
s.set_part_hole_fit(true);                     // 中等产物按洞分片（默认开，ROUND17）
s.set_lvl_cliff(4); s.set_lvl_dirty_pct(30); s.set_lvl_dirty_big_pct(50);   // 时机旋钮：默认即可，别轻易动（ROUND17 §2.3）
u64 dv = s.durable_vers();                      // 已落盘版本；commit 返回后 == last_commit()
s.set_merge_cheap_pages(1024);                 // 读优先 cheap 合并（configure_deploy 已开）
s.shadow_dropped(); s.maint_stats();           // 观测
s.set_lazy_liveness(true);                     // 挂载不算存活账
s.set_sweep_cells(64);                         // 每单元补算未知账的单元格数
s.set_key_index(true);                         // 精确键索引
s.set_io_timeout_ms(200);                      // 有界等待 ⇒ TIMEOUT
s.needs_recovery();                            // 上次提交失败待恢复
s.consolidate(N);                              // 归一：N=0 全库；N≥2 只压碎片单元（polish = consolidate(0)+shrink）
s.compact_file();                              // 归一+搬家+检查点+尾部回落
s.shrink();                                    // 只做尾部回落

s.scrub(&bad, &first_bad);                      // 介质校验（绕过页缓存逐页 CRC）
s.verify(&rep);                                 // 结构/存活账/页所有权全检（rep.first_err / first_key）
s.verify_ownership(&rep);                       // 只查页所有权：被两个主人宣称（owned twice）/ < tail 无人认领（orphan）
s.trace_mark(op); s.trace_dump(stderr, 200);    // 追踪环：分配/释放/提交/回退/搬迁最近 N 条（24 B/条，4096 条）
s.checkpoint();                                 // 归一 + 清单/元数据检查点 + 缺失 meta 自愈
s.heal_missing_meta(512);                       // 给"没有 meta 的活 run"原地补写（页预算）
s.set_rmeta_mode(false);                        // 关掉 run 元数据页（省 ~3.25 B/记录）
s.set_rmeta_min_pages(32);                      // 或只给大 run 写
s.set_overflow(false);                          // 关掉溢出页：>3000 B 的 put 返回 RANGE
s.set_prefetch(false);                          // 关掉扫描预读（多读字节换少 pread）
s.commits_merged();                             // 自动组提交省掉的物理提交数（观测）
s.live_page_count();  s.free_page_count();      // 活页 / 空闲页（高水位 − 空闲）
```

容量与并发（本轮新增）：单库 **16 TB**（`PageId` 32 位，2^32 页 × 4 KB）；
写路径一把 `std::recursive_mutex` + **自动组提交**（`commit()` 发现"没有新写"就直接返回，
把物理提交让给别人）；扫描按 8 页窗口 `read_multi` 预读。

## 目录

```
include/            common / blockdev / layout / store(v2) / fcs(v3)
tests/              回归套件（含 golden/）；Makefile 全量 `make test`
bench/              bench2–7、ab.sh、eval.sh；probe/ 仅 layout.cpp（其余见 backup/r18/swept/）
docs/               DESIGN*、EVAL17、BATTLE、ROUND13–18（索引见 docs/README.md）
web/                动画页源码；根目录 fcs-anim.html / fcs-pits.html 为构建产物
backup/             各轮快照与清扫归档（r18/swept/CLEAN_MANIFEST.md）
```

## 构建与测试

```sh
make          # 全部测试
make t3       # 单个测试
make bench    # v3 写放大矩阵 + 组提交 + v2/v1 对照 + bench6（点读扩展/大值热点/长删循环/压实曲线/文件写合并 fwrite）
make asan     # ASan+UBSan
make dbg      # 打开断言与追踪宏
```

单独跑对比基准：

```sh
g++ -std=c++17 -O2 -Iinclude bench/bench4.cpp -o b4 -lsqlite3
./b4 50000 500 32 50000 2000 100      # 键数 提交批次 值长 点读次数 扫描次数 每次行数
# 之后的位置参数：kidx async group lvl_t async_min consolidate（例：1 1 1 8 0 1）
```

## 空间耗尽与长跑（两条硬要求）

- **绝不挂死**：灌满、写失败、异步 worker、快照/压实并发、设备卡住，都必须返回明确 RC
  （`NOSPACE`/`IO`/`BUSY`/`TIMEOUT`）。每个路径都有 15 s 看门狗守着（`tests/r2.cpp`）；
  打开 `set_io_timeout_ms()` 后任何等待都有上界（卡 3 s 的设备 ⇒ 提交 150 ms 返回 `TIMEOUT`），
  失败只打脏标记、下一次调用再按盘恢复，失败与恢复都不会阻塞调用方。
- **绝不漂移**：页数、run 数、空闲碎片、`mem_bytes`、后台线程都有界且可回落；
  `shrink()` 之后库还能长回来。长跑用 40k 次混合操作守着（`tests/s2.cpp`）。

三条设计规矩（详见 DESIGN3 §5.7 / §8.2）：用户写入只用 7/8 容量，其余留给维护；
**不增长的批**（覆盖/删除）可以透支预留区，所以 `NOSPACE` 永远有出路；
提交是原子的（先落页再动账，失败回到上一个提交点，`mem_` 保留可重试）。

## 当前状态（2026-10-20，第 17 轮之后）

第 17 轮（`docs/20-rounds/ROUND17.md`）：三项全做。① **fsync 出锁**（DISCUSS15 §3 实现版，默认开、无开关）：提交分三段，两次 `sync` + SB 写在 **XLock 外**按票号串行，锁内只剩 `flush_mem` 与抓 SB 载荷；
`commit()` 返回仍 ⇒ 持久，新增 `durable_vers()`（已落盘的最新版本）。② **合并时机规则**：A/B 做完是**否定结论**（悬崖 2/3、门槛 25%、大前缀 30% 全比老规则差）⇒ 默认不变、三个旋钮保留；
调研抓到的真正元凶（大合并刚结束时落尾的 <1024 页中等产物）用"**中等产物按洞分片**"（`set_part_hole_fit`，默认开）治了。③ **家族虚拟源游标**：同 seq 的片串成一条链、一链一游标，
归并宽度 = 家族数；顺带修了老头**源 > 64 条时 live 账漂移**的真 bug（famw 400k/16/8 老头 verify CORRUPT）。`make -j2 -s test` **48 段全绿**（新增 `g17` 8 段）；`make -s asan` **`ASAN_MAKE_EXIT=0`**（≈22 min，`backup/r17/asan.log`）。

| 项 | 第 16 轮 | 第 17 轮 | 复现 |
|---|---|---|---|
| 慢 sync 盘上提交期间一次 get（sync 睡 40 ms） | 79.3 ms | **0.01 ms** | `build/fsync_block 40 5`（新老头都能编） |
| `stall` 读者单次 get max / p99.9 | 3.9~7.7 ms / 68~78 µs | **3.3~3.7 ms / 47~52 µs** | `build/stall 100000 100 4 0` ×3 |
| 合并时刻峰值 layout 200k / 800k（pct 0） | 1.90~2.00× / 1.40× | **1.75~1.81× / 1.36~1.37×**（设备写不变；mode 1 有时间预算 ⇒ 运行间 ±0.1× 噪声） | `build/layout 200000 500 32 1 1 0 0`（≈2 s；800k ≈6~15 s） |
| bench4 200k 末态 space（pct 0 / 150） | 17.5 / 13.2 MB | 17.5 / 13.2 MB（按洞分片在此负载不触发） | `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 {0,150}` |
| 归并游标数（famw 400k, 片 16, lvl_t 8） | = 源数 151（且账漂移 ⇒ verify CORRUPT） | **8**（verify OK） | `build/famw 400000 500 16 8 64`（≈7 s） |

> 取舍明说：① 锁内还有 `flush_mem`（几百 µs），组提交 / 双内存表按拍板不做；阶段二失败窗口从 µs 变成一次 fsync（内存已切新视图、盘上旧提交点 ⇒ 按盘重建）。② 的旋钮默认全是老值，ROUND16 §4.1 "脏 L2 攒着等 4t"的归因作废（悬崖时 L2 仅 ~20% 脏）。
> ③ 在 layout / bench4 默认负载里量不到收益（L3 家族从不互并，源 ≤ 32）；价值是正确性 + 为更小的片扫路。按洞分片让 run 数 +1~2（第二片常是几十页的尾巴）。
> 下面是第 16 轮之后的状态记录（保留）。

### 第 16 轮之后（2026-10-20）

第 16 轮（`docs/20-rounds/ROUND16.md`）：**P2 格式 v2**。P2a：超级块 `'FCS2'` 48 B 头、清单项 48 B（seq/ver 64 位，回绕闸退役为"越过 2^32 不回绕"的断言）、
SB 内联增量 32 → 80 条、`M_RUN_TRIM = 7`（新）/ `M_RUN_HOLE = 8`（只预留）；**老库（v1）可写挂载即升级（惰性：第一次写 SB 就是 v2，不可逆），只读不升级，`fmt > 2` 返回 `UNSUPPORTED`**；
golden 镜像重出为 v2（老三件套在 `tests/golden/v1/`，h2 两套都跑）。P2b：分片归并 —— 大合并（产物 ≥ 1024 页）的产物按片独立成 run 落地、每片入账时给每个源一条 `M_RUN_TRIM`
释放已归并前缀（第 0 页留作身份锚），下一片的预留推迟到提交点之后、按当前最大的洞挑片大小（`set_merge_part_pages`，默认自适应）。
`make -j2 -s test` **47 段全绿**（新增 `g16` 6 段；编译 + 全跑 ≈ 4.5 min）；`make -s asan` **`ASAN_EXIT=0`**（21 min 41 s）；日志 `backup/r16/{test,asan}.log`。

| 项 | 第 15 轮 | 第 16 轮 | 复现 |
|---|---|---|---|
| 合并时刻峰值 layout 200k / 800k（pct 0） | 2.60× / 1.56× | **1.90× / 1.40×**（floor 32 变体 1.84 / 1.37；设计稿预估 1.5 / 1.15 **未达**） | `build/layout 200000 500 32 1 1 0 0`（≈2 s；800k ≈10 s） |
| bench4 200k 末态 space（pct 0） | 22.0 MB / 活 9.0（2.44×） | **17.5 MB（1.94×）**；块层写 18.0 → 18.0 MB；延迟差在噪声内 | `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0` |
| 末态 run 数 layout 200k / 2M | 11 / 30 | 22 / **196**（家族 ≤ 64 片；层规则按家族计数，读按单元不受影响） | 同上，`runs=` 字段 |
| 墓碑规则 | 坑 94 `older_may_contain` | `older_may_contain_grp`（老规则在 g16 `tomb` 段复活 3 键） | `build/g16 tomb` |

> 取舍明说：峰值没到预估的两个原因都量出来了（ROUND16 §4.1/4.2）：合并开始前 8~31 条脏源就占 1.3~1.5×（层规则的事，不在本轮）；分片复用源空间要先"养洞"（N 源 × 一片的起步长尾，
> 与库大小无关 ⇒ 小库上显眼、大库上接近"≈ 一片"）。代价是 run 数变多、归并宽度 = 家族展开后的源数。mode 0 / 不提交就不复用；长快照下峰值回到老样子。
> 下面是第 15 轮之后的状态记录（保留）。

### 第 15 轮之后（2026-10-20）

第 15 轮（`docs/20-rounds/ROUND15.md`）：③ 高水位回收**二步搬家**（改引擎，`hw2_plan` / `hw_move_run` / 回避窗 / `MaintStats.hw_blockers`）；② fsync 出锁只出讨论稿 `docs/30-discuss/DISCUSS15_FSYNC.md`（关键发现：未提交写本来就对读者可见，出锁不改可见性模型，改的是页复用闸与 SB 顺序；等拍板）；① P2 格式 v2 设计稿 `docs/10-design/P2_DESIGN.md`（`'FCS2'` + 48 B 清单项 + seq/ver 64 位 + `M_RUN_TRIM` 分片归并 + `M_RUN_HOLE` 稀疏 run + 老库自动升级；等审后单开实现）。
`make -j2 -s test` **46 段全绿**（新增 `g15` 3 段；4 min 14 s）；`make -s asan` **`ASAN_EXIT=0`**（19 min 53 s）；日志 `backup/r15/{test,asan}.log`。
修正 ROUND14 的一个说法：提交锁内时间本沙箱实测 ≈2~3.5 ms，大头是 `flush_mem`（页编码 + 发写），两次 fsync 只占 1/4~1/3（单次 fsync p50 0.25 ms）；"读者地板 5~6 ms = 两次 fsync"不成立，fsync 出锁在本沙箱收益约 1 ms，慢盘上才大（DISCUSS15 §1）。

| 项 | 第 14 轮 | 第 15 轮 | 复现 |
|---|---|---|---|
| bench4 200k `hwpct=150` 末态 | 17.6 MB / 活 8.9（**1.97×**） | **13.3 MB / 活 9.5（1.40×）**；测量点 runs 10 → 17（搬家作业占作业槽，瞬态 get +24%） | `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 150` |
| hwmap 矩阵 pct 110/130/150/170 | 1.95 / 2.40 / 1.97 / 1.96× | **1.16 / 1.31 / 1.40 / 1.47×**（块层写 37.2 / 35.9 / 34.4 / 40.4 MB，老头 37.1 / 33.0 / 36.4 / 36.4） | `build/hwmap 200000 2000 32 pct 60` |
| layout 800k pct=150 | 1.53× / 写 169.1 MB / 峰 1.56× | 1.54× / 178.4 MB / 峰 **1.81×**（阈值贴稳态 ⇒ 噪声触发；pct=170：170.4 MB / 峰 1.57×）⇒ 调参规则 pct ≥ 稳态 + 20% | `build/layout 800000 500 32 1 1 0 {150,170}` |

> 取舍明说：二步搬家只治合并**后**的静态占用，合并时刻的 2× 峰值仍是格式问题（P2b 分片归并）；策略默认关；轨迹对细节敏感，判据看矩阵不看单格（ROUND15 §1.3）。
> 下面是第 14 轮之后的状态记录（保留）。

### 第 14 轮之后（2026-10-20）

> 可视化讲解：`fcs-anim.html`（引擎运行时解剖，11 章）与 **`fcs-pits.html`**（109 个坑逐条：左老右新同负载动画对比 + 真实 diff + 七节讲解；
> 115/115 全部完整：坑 1~109（文档跳号 58）+ 第 14 轮 7 条发现，每条含左老右新动画、代码对比与七节讲解）。两者都是无外部依赖的单文件，源码与构建见 `web/README.md`。


第 14 轮（`docs/20-rounds/ROUND14.md`，三项全做）：(1) 分步作业收尾改**单元级增量账**（不再把 touched 单元全标未知）⇒ mode 1 预算真正有效；
(2) 文件高水位：流式预留按存活字节估 + 估小就地续租 + 顶部释放 `tail_` 回落 + 清单页簇 + 可选回收策略 `set_hw_reclaim_pct`；
(3) mode 3 读者让路：维护单元 1 ms 时间片 `set_maint_quantum_us`。`make -j2 -s test` **45 段全绿**（新增 `g14` 6 段；5 min 13 s）；`make -s asan` **`ASAN_EXIT=0`**（26 min 48 s，含新加的 `tsan_g14 yield`）；日志 `backup/r14/{test14,asan14}.log`。

| 项 | 改前（第 13 轮） | 改后（默认） | 复现 |
|---|---|---|---|
| wprof_lt mode 1 预算 20 ms（10 万键 × 500 B，150 × 2000 覆盖） | avg 28~31 / p90 32 / **max 382~549 ms**，欠账 4.7~5.2 s | avg 15~16 / p90 31~33 / **max 37~44 ms**，>50 ms 0，**欠账 0**（WA 1.8 → 1.94×） | `build/wprof_lt 100000 500 150 2000 8 0 1 -1` |
| stall 读者 p99（写者定节拍 50 ms） | mode 1 p99 36 µs / get max 22.6 ms；mode 3 **p99 60 µs** / >1 ms 412 | mode 1 **p99 20 µs** / max 5.7 ms；mode 3 **p99 17.6 µs** / >1 ms 222；读者吞吐 ×2.1~2.3 | `build/stall 100000 100 4 0 0 {1,3} -1 50` |
| 合并时刻峰值 layout 200k（+10 万覆盖） | **2.95×** 活数据 | **2.60×**（800k：1.88 → 1.56×，末态 1.74 → 1.53×） | `build/layout 200000 500 32 1 1` |
| 合并后静态占用 bench4 200k | hw 21.3 MB / 活 9.0（2.4×） | 默认 22.0 MB（2.44×，**现格式不可避**，ROUND14 §1.2.0）；`hwpct=150`：**17.6 MB**（块层写 +22%）；layout 探针 1.16~1.40× | `build/bench4 200000 2000 32 50000 2000 100 0 0 0 0 0 0 0 1 0 150` |

> 取舍明说：顶层合并的产物要连续区间、源 run 的页要到下个提交点才释放 ⇒ 峰值 ≈ 活 + 产物，这是格式问题（P2 `M_RUN_TRIM` / 顶层分区）；
> 读者停顿地板 5~6 ms 是提交期两次 fsync 在排他锁内（移出去 = 可见先于持久，语义变更，未做）；回收策略默认关（用写换空间）。
> 下面是第 13 轮之后的状态记录（保留）。

### 第 13 轮之后（2026-10-20）

第 13 轮（`docs/20-rounds/ROUND13.md`）：① 提交点重编号 ② 归并 / 归一产物流式落盘 ③ 读放大触发的层合并 ④ mode 1 预算 + mode 3 后台线程
⑤ 按脏度合并 / 重写（含归并丢组外被盖记录、范围删除计脏）。`make -j2 -s test` **44 段**全绿（新增 `g13` 7 段）；
`make -s asan` **ASAN_EXIT=0**（第一遍 TSan 抓到坑 109 = 观测接口无锁读，修后第二遍全绿；ROUND13 §7）。收尾时 g6 抓到 **坑 104（`shrink()` 在提交点之外的维护之后掉电丢已提交数据，第 12 轮的头就有）**，已修。

| 项 | 改前 | 改后（默认） | 复现 |
|---|---|---|---|
| bench4 50k 纯插入后 run 数 / 区间扫描 vs SQLite | 26 条 / 3.9 ~ 4.3× 慢 | **10 条 / 1.0 ~ 1.6×**；点查 0.34 → 0.22× | `build/bench4_c{0,1024} 50000 500 32 50000 2000 100` |
| bench4 200k | 26 条 / 6.35× / 提交 max 50 ms | 10 条 / 2.3× / 提交 max 7 ms；**高水位 14.6 → 21.3 MB（活 9.0）** | `… 200000 2000 32 50000 2000 100` |
| 写放大 bench3r | 2.63 / 2.61 / 2.10× | 2.98 / 3.48 / 2.17× | `build/bench3r_c{0,1024} {20000 500, 200000 2000, 200000 2000 8}` |
| 提交延迟 wprof_lt（10 万键 × 500 B） | mode 1 不限预算 p90 108 ~ 116 / max 156 ~ 159 ms | mode 1 预算 20 ms **p90 29 ~ 30** / max 221 ~ 230；**mode 3 avg 4.7 / p90 6.5 / max 12 ms** | `build/wprof_lt 100000 500 150 2000 8 0 {1 -1, 1 0, 3}` |
| 读者停顿 stall | commit max 208 ~ 268 ms、get max 13.8 ~ 17.8 ms | mode 1：commit max 26 ~ 31、get max 9 ~ 11 ms；mode 3：commit max 8 ~ 14、get p99 59 → 85 µs | `build/stall 100000 100 4 0 0 {1,3}` |
| 归一峰值内存（149 MB 库） | +262 MB | **+0 MB** | `build/consol_rss 300000 500 10000` |
| seq 回绕 | 用尽前 FULL | 自动重编号，回绕 400 次全对 | `build/seqwrap 50 400` |

> 取舍明说：(c) 规则是读优先，写放大 +13 ~ 33%、文件高水位（连续产物装不进洞）200k 时 2.4× 活数据——写多读少就
> `set_merge_cheap_pages(0)`；mode 1 预算 20 ms 的 max 尾巴（220 ~ 570 ms）还在——根因是分步作业收尾把全部单元标未知、补算吃光预算、L0 撞 4t 悬崖被兜底一口气并掉（ROUND13 §5，下一轮第一项）；要硬上界用 mode 3（多一个线程、读者 p99 变差）。
> 下面是第 12 轮之后的状态记录（保留）。

### 第 12 轮之后（2026-10-19）

全部测试通过：

```
t1..t6 / tchk / x1 / x2       v2 全套
f1  语义 + 随机对拍（每轮全量 verify + 重开）           PASS
f2  快照视图 / O(1) 回退 / 重开一致                      PASS
f3  NOR 语义 + 逐写入点掉电注入 + CRC 损坏检测           PASS
f4  5 万次操作 + 多轮提交重开 + 随机对拍                 PASS
f5  空间回收：归一/搬家/检查点/收缩 + 逐键对拍 + 重开    PASS
f6  存活账精确性：覆盖型负载，每次提交都 verify（段级记账）  PASS
q1  游标 / 分页 / 前缀 / 二级索引 / 快照扫描 / 组提交 / 异步写  PASS
p1  页内前缀压缩：有序键省字节、随机键零回归、压缩页可改可读  PASS
r2  空间耗尽 / 写失败 / 异步灌满 / 回退持久 / 卡住的设备        PASS
s2  长跑无漂移：40k 混合操作，页数/run/碎片/内存全部有界可回落  PASS

── 本轮新增 ──────────────────────────────────────────────────────
v1  溢出值 1B~65000B / 覆盖 / 压实 / 重开 / 空间不泄漏            PASS
g1  范围墓碑：删中段 / 删后再写 / 重开恢复 / 归一后物化            PASS
w1  多写者 4 线程 × 5000 + 自动组提交（commits_merged>0）         PASS
s3  快照存活期间回收不停摆（老快照 0 错、verify 0）                PASS
c1  scrub 绕过缓存查介质（翻转 1 bit 必被抓到，重开仍报）          PASS
b1  >256 MB 大库（32 位页号，两段负载，1.56 GB 设备）              PASS
s4  两个重叠快照 + 11 轮覆盖：年长/年轻/当前三视图各 0 错          PASS
g2  5000 个范围墓碑：点查 0.25 µs、存活账不漂移、归一后 15000 条    PASS

── 第 7 轮新增 ────────────────────────────────────────────────────
g3  同 lo 墓碑 / 长删循环 run 退休 / 未提交大值 get / 链缓存失效       PASS
g4  异步写×快照回退：预取在飞页 / pend_delta_ 顺序 / 快照含逻辑提交 /
    压实后 used == live（6 轮搬家 0 碎片）                              PASS
s2  （加强）每次回退后与快照时刻真值**逐键比对**（以前只 resync）      PASS

── 第 8 轮新增 ────────────────────────────────────────────────────
g5  交错模糊：put/del/del_range/get/select × commit/commit(false)/snapshot/
    rollback/forget/维护/scrub/reopen 随机交错，逐键模型 + 页所有权，4 种子   PASS

── 第 9 轮新增 ────────────────────────────────────────────────────
g6  掉电注入：VolDev 在第 k 次页写/sync/截断处断电，重开后逐键比对上一个
    持久提交点（4 种子；首跑抓到坑 81~84）                                PASS
g7  读写并发：1 写者 op 流 × N 读者 get/select 不变量校验，共享读 / 独占读 /
    4 读者三种配置；`make asan` 里再以 TSan 跑三遍（坑 85、86）             PASS

── 第 10 轮新增 ───────────────────────────────────────────────────
g8  覆盖风暴 × 多单元 × 层合并 × 快照钉住：每次提交后**逐键全量比对模型**、持快照期间比对
    快照视图、重开再比对、run 数/段数/层级上界（3 组；老引擎第 18/51 次提交就读到过期值）   PASS

── 第 11 轮新增 ───────────────────────────────────────────────────
g9  坑 92~95 回归（悬空墓碑钉 run / 快照期墓碑支配 / 点墓碑永不清 / 存活口径）+ 坑 94
    三段（队列负载 recs→0、快照期不丢、压实摘段后老值不复活）；老头文件编：t92/t93/t94/t95 全 FAIL   PASS
g10 R2 层合并预算 + 分步维护（每步 verify + 快照视图抽查）+ R7 墓碑自动归一 + R8 局部 TRIM      PASS
h1  挂载模糊：合法镜像随机翻 1~16 字节 × 400 次，open 只许 OK/CORRUPT/IO，假版本必须被 verify 查出   PASS
h2  golden 镜像兼容：tests/golden/*.img 只读挂载 + 逐键 + 快照视图 + verify（格式升版时的护栏）     PASS
h3  多写者 × 异步提交 × 掉电：3 写者线程 + VolDev 随机掉电/撕裂，按每线程自己的持久下界比对
    （长 span 攒合并 + 短 span 打提交点窗口；去掉 SB 前的 sync 时 3 种子都抓得到）                   PASS
g7-40  40 读者 > 32 线程槽（槽溢出走独占路径）                                                      PASS

── 第 12 轮新增 ───────────────────────────────────────────────────
g11 提交后维护（mode 0/1）× 切片预算（0/3/24）矩阵：每次提交 verify + 全量比对、每 15 次重开；作业被
    提交打断（units_max=2 ⇒ 作业必跨提交）；NOSPACE 下坑 99 回归（320 页小设备：成功 29 / NOSPACE 91，
    重开单调）；4 读者并发下 mode 1 维护；三种模式的惰性存活账（重开全未知 → 30 次提交后归零 → verify）   PASS
g12 坑 102：70000 次逻辑提交 + 第 65000 次后持有快照跨过第 65536 次 ⇒ 视图 / verify / 回退 / 重开全对
    （老头：视图 2000/2000 键看见未来写、verify 误报 future ver 2539 处）                                  PASS
```

── 第 13 轮新增 ───────────────────────────────────────────────────
```
g13 流式产物（含故意估小预留逼搬家、异步、NOSPACE 退路）/ 提交点重编号（快照期不重编、作业放弃、掉电两种落点）/
    层合并规则 (c)(d)(e)(e2)(f)（run 峰值、垃圾 0%、顶层 422 → 106 页、lvl 1 单条被范围删除盖 60% → 337 页）/
    mode 1 预算（1 µs × 48 次提交推迟 41 次、放开后欠账清零）/ mode 3 线程 + 4 读者 + open() 换设备                 PASS
```

── 第 16 轮新增 ───────────────────────────────────────────────────
```
g16 P2b 分片归并 + RUN_TRIM：分片基本面（150 次提交 parts=201 trims=673、清账后家族并掉、账零漂移 + verify + 比对）/ 跨重开（作业中途
    关库 ⇒ 重放 TRIM、所有权、读全对；重开后裁过的源 4 条，继续维护并掉）/ abort() + rollback(快照)（状态与清单一致）/ 分片中途开快照
    （作业作废、快照读不变）/ 溢出链（1/3 大值 parts=6，重开 + 附件页过滤 + verify）/ 墓碑（作业作废后删片键域 150 键再合并：复活 0；
    老规则实验头复活 3）                                                                                                           PASS
h2  改为 v2 golden 三件套 + v1 老库三件套（只读不写回 → 写即升级 → 重开 mounted_format()=2）；g13 的 stream/renumber 段改写为 seq64 /
    v1chain（白盒造 v1 清单链 + 掉电 ⇒ 混链重放）
```

共 **47 段**（第 15 轮 46 段 + g16）；本机沙箱 `make -j2 test` 编译 + 全跑 ≈ 4.5 min（已编好只跑 44 s），`make asan` **21 min 41 s**。

── 第 15 轮新增 ───────────────────────────────────────────────────
```
g15 高水位回收二步搬家：规划器（窗 [67,353) 挡路 run，无窗外目的地时放弃）/ 二步搬家（run 布局顶 568 → 282、挡路 2 条、顶 1 次、
    4 个提交点）/ 快照钉住挡路 run（存活期间不动、释放后 → 282；抓出 hw2_gen_ 被空提交永久闸死的 bug）                        PASS
```

共 **46 段**（第 14 轮 45 段 + g15）；本机沙箱 `make -j2 test` 实测 **4 min 14 s**，`make asan` **19 min 53 s**（第 14 轮 26 min 48 s；机器空闲程度不同）。

── 第 14 轮新增 ───────────────────────────────────────────────────
```
g14 作业收尾增量账（120 次提交 job_fast=14212 job_slow=0、欠账 0、所有已知单元账与 recount 零漂移）/ 预留按存活估（80% 脏 run
    重写：峰值增长 1.26× 产物，老估法 5.6×）/ 估小就地续租 2 次 0 搬家 / 高水位回收（pct=0 不回收；pct=150 mode 1/3：1.58× → 1.03×）/
    顶部释放 tail_ 回落（白盒 + 清单页簇不回落 + 重开）/ mode 3 读者让路（quantum 1 ms 让路 37 次、quantum 0 让路 0 次、账零漂移）   PASS
```

共 **45 段**（第 13 轮 44 段 + g14；按 Makefile 的 `==` 行计）；本机沙箱 `make -j2 test` 实测 **≈ 5 min**（4 min 55 s ~ 5 min 13 s），`make asan` **≈ 27 min**。

> 第 13 轮时的说明（保留）：共 **44 段**（第 12 轮 43 段 + g13；按 Makefile 的 `==` 行计）；本机沙箱 `make -j2 test` 实测 **≈ 4.5 min**。

> 第 12 轮时的说明（保留）：共 **43 段**（按 Makefile 的 `==` 行计：v2 的 9 段 + v3 的 34 段，g7 占三段）；本机沙箱 `make -j2 test` 实测 **190 ~ 260 s**（`s2`/`b1`/`r2` 占大头；
热缓存的本机约 15 s）。`make asan` 另含 TSan：`w1` + `g7` 三种配置 + `g11`，LSan 常开（`detect_leaks=1`），沙箱约 20 ~ 25 min。
新增调试/控制 API（都有默认值 = 老行为）：`set_level_merges(n)`、`consolidate_step()` / `compact_file_step()`（返回 `RC::MORE`
表示还有下一步）、`set_range_tomb_auto(n)`、`set_discard_min_pages(m)`（`FileDev` 打洞）、`range_tombstones()`。
`build/g8 [seed] [nkeys] [commits] [per]`（默认 1 4000 160 200；失败时打印首个出错的提交号、键、读到/期望的序号）。
`build/g6 [seed] [ops]`（`G6_WATCH=<pid>` 看某页在介质上的变化，`G6_LOG=1` 逐步打印）；
`build/g7 [seed] [writer_ops] [readers]`（默认共享读模式，`G7_EXCL=1` 测独占老路）。
`build/g5 [seed] [ops]` 单跑一个种子（种子决定 async / io_threads / arena / 整页写 / 慢写）；
失败时打印种子、步号并倒出引擎追踪环；`G5_LOG=1` 逐步打印操作，`G5_VERIFY_EVERY=1` 每步 `verify()`。

### 第 12 轮的关键数字（M1 → L1 R3-lite → L2 R2b → C2 → M2 R6-lite → C1；C3/C4 否决、T1 实验不进引擎）

全部数字、取舍与复现命令在 `backup/r18/swept/docs-hist/DISCUSS12.md` §6；改前头文件 `backup/r12/fcs.h.pre-r12`。

| 项 | 改前 | 改后（默认配置） | 复现 |
|---|---|---|---|
| **读者停顿** 10 万键 × 100 B，写者每批 2000 put + commit，1 读者 64 get 一批，4 s | 单次 get 最长等待 **133.5 ms**（= 整个提交持锁）、>10 ms 9 次、p50 13.0 µs | **13 ms**（×3：12.6 / 14.0 / 13.0）、>10 ms 2 ~ 4 次、p50 9.3 ~ 10.4 µs | `build/stall 100000 100 4 0`（改前头编 `-I/tmp/incA`，命令见 DISCUSS12 §6.9） |
| **写者** 10 万键 × 500 B，150 次 × 2000 键（A/B 交替 ×3） | avg 23.9 / p50 9.0 / p90 46.5 / max 502 ms | avg 24.4（1.02×）/ p50 8.2 / **p90 90.7（1.95×）** / max 393（0.78×）；写放大 2.57 → 2.56× | `bash bench/ab.sh backup/r12/fcs.h.pre-r12 include/fcs.h backup/r18/swept/bench-probe/wprof.cpp 3 -- 100000 500 150 2000` |
| **挂载** 100 万条 × 50 B（20 runs / 4069 单元） | open 733 ~ 759 ms | open **27 ~ 28 ms**；补算 64 个单元 414 ~ 469 ms 分摊到之后的维护单元 | `build/mount 1000000 50 50000 3 {1,0}` |
| **泄漏** 反复 open/close | 每次 +8 MB | RSS 恒定；LSan 常开无报告 | `build/mount 1000000 50 50000 5`、`ASAN_OPTIONS=detect_leaks=1 build/mount_asan 200000 50 50000 4` |
| **CPU**（gprof 主线程，C2） | 1.46 s | 0.98 s | DISCUSS12 §1.1 的 gprof 命令 |
| **T1** 覆盖型大值负载 `set_lvl_t(4)` vs 默认 8 | avg 28 / p90 100 / max 420 ms、WA 2.56× | avg 17 / p90 43 / max 229、WA 2.46×（小记录负载反过来：2.77 vs 3.53× ⇒ 默认不动） | `build/wprof_lt 100000 500 150 2000 {8,4}`、`build/bench3r 200000 2000 0 {8,4}` |

> 取舍明说：mode 1 把维护挪到提交点之后，读者只等一个维护单元（≤ 13 ms），但 **`commit()` 调用方的墙钟包含维护循环，不降反升**
>（stall 134 → 185 ~ 207 ms；wprof p90 ×1.95）——要调用方延迟就 `set_maint_units_max(8)`（56 ms，维护积压）或 mode 2 起线程调 `maintain()`。
> C3（拆分两半推迟 recount）与 C4（probe_prev 估算）实测否决；T1 三版自适应都没有普适收益，不进引擎。
> 新坑 99 / 99b / 100 / 101 + snapshot/abort 必须 `commit_locked`：`docs/10-design/DESIGN3.md` §12。L3 R3-full、L4 R11 不在本轮；P2 仍单开。
> 收尾的全量评估（**`backup/r18/swept/docs-hist/EVAL12.md`**：正确性 / 性能 / 资源 / 边界 / 风险清单 / 复现矩阵 `bench/eval.sh`）顺手抓到 **坑 102**：
> 逻辑提交版本是 16 位、第 65536 次提交回绕 ⇒ 跨回绕的快照视图失效、verify 误报；已修（引擎内版本 u32，盘上本来就是 32 位，不改格式），回归 g12。
> 同一张边界表还撞出 **坑 103**：run 序号 u32 回绕 ⇒ 覆盖写静默读到旧值（探针 `backup/r18/swept/bench-probe/seqwrap.cpp`）；本轮只加闸——
> 序号用尽前 256 个号 `commit()` 返回 `FULL`（批次仍在内存）；每次 flush 占一个号，1000 次/s 连续 ≈ 37 天到顶；重编号（不改格式）留给下一轮。

### 第 11 轮的关键数字（坑 92~95 → R1 → 坑 94 → R2 → R7/R8 → H1~H5；R3 只出设计，P2 单开）

| 项 | 改前 | 改后 | 复现 |
|---|---|---|---|
| **R1** 10 万键 × 500 B，150 次 × 2000 键（FileDev，同时段交替 ×3） | avg 38.0 ms / p90 99.5 / max 612 ms；user CPU 4.21 s | avg **31.5 ms**（−17%）/ p90 **65.8**（−34%）/ max 517 ms；user CPU **3.28 s**（−22%） | `bench/ab.sh backup/r11/fcs.h.pre-r11 include/fcs.h backup/r18/swept/bench-probe/wprof.cpp 3 -- 100000 500 150 2000` |
| **坑 94** 队列负载（put 5000 / del 5000 / consolidate）× 5 轮 | 记录 5000 → **25000**、页 36 → 180（线性增长） | recs **0**、runs 0 | `g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/tomb_forever.cpp -o /tmp/tf && /tmp/tf` |
| **R2 预算** 2 万键 × 500 B，200 次 × 500 键 | max 75 ms / p90 15.6 | `set_level_merges(1)`：max 61 / p90 **20.7**（p90 变差 ⇒ 默认 0） | `build/wprof 20000 500 200 500 {0,1}` |
| **R2 分步** 30 条 run 的 `consolidate_step` | 一次性 `consolidate(0)` | 4 步 + 1 步收尾，每步 ≤ lvl_t 条 run，每步后 verify + 快照视图抽查 | `build/g10` |
| **R7** 范围墓碑自动归一（阈值 32） | 墓碑只增不减 | 峰值 39 → 第 13 次提交清零；快照期保留 | `build/g10` |
| **R8** 60 次提交 × 1000 × 500 B，高水位 9627 页 | 文件实占 37.6 MB | `set_discard_min_pages(16)`：**13.8 MB**，verify / 回读全对 | `build/g10`（t_discard：13.4 → 5.4 MB） |
| **H1** 400 次随机翻字节 | — | 0 崩溃 / 0 挂死；392 次 open OK、23 次 get 读到假版本，全部被 verify 查出 | `build/h1 400 1` |

> 坑 92/93/94/95 都是「新老同在」的正确性 / 空间缺陷（`docs/10-design/DESIGN3.md` §12），落地前 g9 / g5 又抓到两个新坑（96/97）。
> 取舍明说：R2 的「每提交一次合并」实测 p90 变差、只削中等负载的 max，大负载的 max 是单次顶层合并，预算无效 ⇒ 默认关；
> R10 无收益（保留代码）；R3 提交流水线只出设计稿 `backup/r18/swept/docs-hist/DISCUSS12_R3.md`；P2 格式升版按约定单开一轮。

### 第 10 轮的关键数字（R0 悬崖 + H0 + R9）

| 项 | 改前 | 改后 | 复现 |
|---|---|---|---|
| **R0 悬崖**：2 万键 × 500 B，200 次提交 × 覆盖 500 键 | 提交 avg **100 ms** / p90 395 / max 993 ms，>50 ms 61 次，runs **685**，used 12805 页 | avg **7.9 ms** / p90 27 / max 96 ms，>50 ms 3 次，runs **19**，used 9750 页 | `g++ -std=c++17 -O2 -pthread -Iinclude backup/r18/swept/bench-probe/wprof.cpp -o build/wprof && build/wprof 20000 500 200 500` |
| 同上 320 次提交 | avg 165 / **p50 63.8** / max 977 ms，runs 851 | avg 10.1 / p50 2.5 / max 130 ms，runs 28 | `build/wprof 20000 500 320 500` |
| 10 万键 × 500 B，150 次 × 2000 键 | avg **407 ms** / max **17.3 s**，runs 92（5506 单元 ≈93 段/单元） | avg **37 ms** / max **0.59 s**，runs 11 | `build/wprof 100000 500 150 2000` |
| 覆盖风暴：写放大 / 空间放大 / **回读过期值** | 2.68× / 1.58× / **bad=2**（`5000 500 200 200` 时 **1127**） | 2.68× / 1.27× / **0** | `build/bench6 ovw 20000 500 200 500 0` |
| bench3r 200k/2000 写放大（不压实的负载，对照） | 2.77× | 2.77× | `build/bench3r 200000 2000` |

> 三件事：(1) `count_refs` 的二次方扫描改成 O(1) 引用计数；(2) 压实产物的层号不再无上界、seq 改用新鲜号；
> (3) 层合并组加"全库 seq 连续"检查。(2)(3) 顺手修掉了**两个老的正确性缺陷**（坑 90/91：特定工况下 `get` 返回旧版本，
> 第 7 轮起就在，`verify()` 查不出来，g8 每次提交逐键比对才抓到）。改后残余的 max（96~593 ms）是层合并级联，归 R2。
> 老库升级后建议跑一次 `compact_file()`，把升级前遗留的"同 seq 对"清掉（详见 DESIGN3 §10.17、§12 坑 88~91）。

### 第 9 轮的关键数字（顺手项 → G1 → G2 → P1）

| 项 | 改前 | 改后 | 复现 |
|---|---|---|---|
| **P1 读扩展性**：10 万键单 run 全命中缓存，20 万次 get | 1/2/4 线程 **96.4 / 109.0 / 171.3 ms** | `set_shared_reads(true)`：**87~95 / 52~64 / 54~62 ms**；独占老路 89~97 / 111 | `build/bench6 rget {1,2,4} 100000 4096 1`（第 5 参 0 = 独占） |
| P1 小缓存（64 页，19 万次 pread） | — | 147 / 169 / 173 ms —— **不加速**，瓶颈是沙箱 pread 路径（去掉设备原子统计也一样）；真机预估可扩展，未实测 | `build/bench6 rget {1,2,4} 100000 64 1` |
| **P5 快照期维护**：持 1 个快照，2000 键 × 300 轮覆盖 + `compact_file` | 维护 300 次全 BUSY，高水位 36 → **272 页（7.6×）** | 300 次全 OK，**71 页（2.0×）**；回退逐键 0 错；g5 维护 BUSY 35~40% → 0 | `build/bench6 snapmaint 2000 300 1` |
| **G1 代价**：每个持久提交点 2 次 fdatasync | `fwrite 20000 20 100 0` 0.41 s | **0.47 s**（≈+70 µs/commit，沙箱 fs；真机 fdatasync 翻倍） | `build/bench6 fwrite 20000 20 100 0` |
| **P4** `del_range` 不命中（100k 条内存表） | 142.8 µs/次 | **6.43 µs/次（摊销，首次建视图）**；命中 0.30 µs | `build/bench6 rngmem 100000 1000` |
| **P3 大值链异步**：慢设备（100 µs/写）8 KB put 前台延迟 | **376~399 µs**（前台写调用 2.0/put） | **4.6~6.0 µs**（0/put）；io_threads=1 时 put+commit 合计不变（延迟挪进 commit），=2/4 时合计 202 → **116 / 58 ms** | `build/bench6 bigput 500 8192 100 {1,2,4}` |

> `set_shared_reads(bool)` **默认关**（保持今天的语义：select 回调里可以写）。开了之后：get/get_at/select/scan/index_scan 走共享锁，
> 写与维护仍独占、**写者优先**（坑 86）；持共享锁的线程再进写入口返回 `RC::BUSY`（不升级锁 —— 升级间隙里别人的维护可能
> 回收 run）。进程最多 32 个"快路"线程，再多的线程读也走独占。详见 `docs/10-design/DESIGN3.md` §10.16、§12 坑 81~87。
>
> **P5 语义变化**：`consolidate/relocate/checkpoint/shrink/compact_file` 在快照存活时不再返回 `BUSY`，但只处理
> `ver > 最新快照` 的 run（快照之后写入的垃圾立即回收；老 run 原地不动，它可能钉着尾部）。要让文件长度回落到存活集，
> 仍需 `forget_snapshots()` 之后再 `compact_file()`。

### 第 8 轮（上半）的关键数字（A2+A4+B3 → B4+A3；B1/B2 未动）

| 项 | 改前 | 改后 | 复现 |
|---|---|---|---|
| **B3 分配规则**：长删循环 8 轮 used/live | **1.14**（3133/2746） | **1.00**，8 轮全 1.00 | `build/bench6 delloop 8 100000` |
| **B4 合并写**：FileDev 100 次 commit 的写 syscall | 12296 次 | **2486 次（−80%）**；大批次 10500 → 434（−96%） | `build/bench6 fwrite 200000 2000 100 0` / `fwrite 400000 50000 100 0` |
| **B4 墙钟** | 0.455~0.481 s | 0.429~0.503 s —— **噪声内**（预估 −20~30% 落空：pwrite 只有 1~2 µs） | 同上 |
| **写放大**（RamDev 口径） | 2.77× | **2.77×**（`whole_pages()` 分叉；首版整页写曾变 2.83×） | `build/bench3r 200000 2000` |
| **A3 `g5`** | — | 4 种子 ×12000 步 **≈3 s**；1~60 种子、100k 步长跑全绿；**首轮抓到 7 个 bug** | `build/g5`；`build/g5 3 100000` |
| **读己之写的代价**：100k 条未提交 + 写/查交错 | （select 看不见内存表） | 第一次 select 排一次 5~6 ms；交错 **0.06 ms/次**（增量归并） | `docs/10-design/DESIGN3.md` §10.15 `selcost` |
| **维护 = 提交点的代价**：s2（每 ~16 步一次维护） | CPU 2.80 s | **3.37 s（+20%）**，全部来自维护前的 commit | `build/s2` |

> `g5` 第一轮抓到的 7 个 bug（`docs/10-design/DESIGN3.md` §12 坑 74~80）：`shrink` 截断未提交大值的链；`del_range` 之后同批 `put`
> 丢失；`del_range` 提交前 `get` 看不见；`select/count/scan` 不读内存表；维护操作把未提交墓碑"半持久化"；
> 范围墓碑下点墓碑的账差 4 B；重放时同 `(seq,page0)` 的段删除串代（reopen 后 352 键消失）。
> **语义变化**：① put/del/del_range 对 `get`/`select`/`count`/`scan` 立即可见；② `consolidate/relocate/checkpoint/compact_file`
> 先 `commit()`（维护 = 提交点，`shrink` 除外）；③ 范围墓碑只盖**严格更早**版本的记录。

### 第 7 轮的关键数字（度量 → L1/L2/L4 → M1）

全部为沙箱实测，old/new 同时段各 ≥3 次取中位；复现命令在右列（`make bench` 也会跑 bench6 四段）。

| 项 | 改前 | 改后 | 复现 |
|---|---|---|---|
| **大值库 `compact_file`（M1）** | ratio **1.31**（used 26541 / live 20236） | ratio **1.01**（used 20502）；代价 cf 365 → 550~820 ms | `build/bench6 cfcurve 20000 3900 4` |
| **`b1` 大库 compact_file** | 96905 → 127795（**+31%**） | 96905 → **97649（+0.8%）** | `build/b1` |
| **长删循环（L4）** | del 提交 2.4→**68 ms**、回填 445~640 ms、runs 40→138 | del 提交 **0.7~0.9 ms**、回填 **20~25 ms**、runs 恒 20 | `build/bench6 delloop 6 100000` |
| **put（L1 内存表）** | 0.117~0.125 µs/op | **0.022~0.026 µs/op（5×）**；`bench3r` 总时 −13%，写放大字节不变 | `build/bench3r 200000 2000` |
| **大值热点读（L2）** | hot=16：28~33 ms、pread 20001 | **5.6~6.4 ms、pread 33**；工作集 > 缓存时持平；冷读无回归 | `build/bench6 bigget 16 20000 8192` / `bigget 64` |
| **快照期接管** | 持有快照时链一律重写 | 快照期 60 批覆盖写页 **63539 → 45389（−29%）**，回退 0 错 | §10.14（`snaptake`） |
| **写放大** | 2.77× | **2.77×**（逐位不变） | `build/bench3r 200000 2000` |
| **压实接管** | 1.4~1.7 ms | **1.4 ms**（没动） | `build/bench5 consol 1` |

> 顺带修了三个**潜伏两轮的旧 bug**（都在异步写 × 快照/回退的交叉处，`g4` 固化）：
> ① 预取绕过 `wait_page`，`commit(false)` 后立刻 `select` 读到旧页并污染页缓存（旧版 3000 行只回 80 行）；
> ② 后台刷出的 run 的 `M_RUN_ADD` 晚于合并它的 DEL 入账 ⇒ 重放复活僵尸 run（s2 里丢 14 键 / 回退 CORRUPT 清库）；
> ③ `snapshot()` 不含逻辑提交 ⇒ 回退丢已可见的写。s2 以前每次回退后 resync 真值，把它们全藏住了。
> 详见 `docs/10-design/DESIGN3.md` §12 坑 67/68/69/72。**语义变化**：`snapshot()` 现在先 `commit()`（快照 = 持久提交点）。

### 本轮（第 6 批）的关键数字

| 项 | 数字 | 复现 |
|---|---|---|
| **压实零写接管** | `consol`：**66.8 ms → 1.7 ms**；读页 **2004 → 4**；写页 **6023 → 23**（写放大 −99.6%） | `build/bench5 consol 0` / `consol 1` |
| 大值顺序读 | 2000 × 8192 B：3.1~6.1 ms、**pread 6050 → 2000**（8 页一批）、24.6 MB | `build/bench5 bigscan 64 2000 8192` |
| 快照期空间 | `used` 44 → **74 页（1.7×）**（上一版 14.1×） | `build/s3` |
| 多快照正确性 | 三份视图各 **n=2000 / bad=0**；`rollback` 后仍全对 | `build/s4` |
| 范围墓碑 | 5000 个墓碑：get **0.25 µs/次**、插入 4~6 ms、`verify` 0 | `build/g2` |
| 容量 | 256 MB → **16 TB**；503 MB 库重开 152 ms、`verify` 0 错 | `build/b1` |
| 值上限 | 3992 B → **65534 B**；8 KB 值读 **1.7~2.0 GB/s** | `build/bench5 big 8192 2000` |
| 扫描预读 | pread **317 → 58**（128 页 arena，读字节不变） | `build/bench5 scan 128 20000 1/0` |
| 多写者 | 1→4 线程 **1.4~1.5×**（`flush_mem` 还在锁里） | `build/bench5 mt 1 20000` / `mt 4 5000` |
| 快照期间回收 | 见上方「快照期空间」：44 → **74 页**（1.7×），老快照 0 错 | `build/s3` |
| meta 自愈 | 清掉 20 个 meta → 重开 hits=0 → `checkpoint()` → **hits=20** | `build/m1` |
| 写放大 | **一位没动**：3.63 / 2.70 / 2.35 / 2.77 / 2.23 | `make bench` |

> 大值的两个代价写在 DESIGN3 §10.8 / §10.13：8192 B 值要 3 个溢出页（空间放大 **1.5×**，
> 8160 B 正好 2 页 = 1.00×）；每个含大值的 run 多 2 页 `AOVL` 附件（小库 33 → 35 页）。
> **压实不再重写大值**（§10.13 的"零写接管"）；`compact_file()` 的 +31% 硬伤（§12 坑 61）
> 第 7 轮已解：第二遍只物化高位链，`b1` 变成 +0.8%，代价是 compact_file 耗时 2.3×（§10.14）。

写放大完整矩阵见 `docs/10-design/DESIGN3.md` §10.1（随机键，值 4–28 B）：
20000/100 **3.63×**、20000/500 **2.70×**、50000/1000 **2.35×**、200000/2000 **2.77×**、
200000/2000（8:1 组提交）**2.23×**。这组数字是**本批改动之后**重测的，与上一版逐位相同。

对照（20000 put，值 4–28 B，每 500 提交）：

| 引擎 | 写字节 | 写放大 | 用时 |
| ---- | -----: | -----: | ---: |
| v1 | 17.44 MB | 56.2× | 0.25s |
| v2 (NAV_CAP=64) | 6.99 MB | 22.5× | 0.08s |
| v2 (NAV_CAP=512) | 5.93 MB | 19.1× | 0.07s |
| **v3（当前）** | 1.56 MB | **2.70×** | 0.006s |

> v1/v2 的数字来自同一负载的历史测量（`bench/bench_v1.cpp` 那一套），v3 行是本次运行的
> `bench3` 结果；两边的"写字节"口径一致（都只算设备写入）。

（历史）上一批的两处改动把 `200000 / 2000` 从 3.14× / 0.95 s 拉到 **2.57× / 0.36 s**：
分层合并的垃圾门槛 `FCS_LVL_DIRTY_PCT` 从 20% 提到 30%（20% 时一堆 run 刚写完就被覆盖、
反复重写），清单快照页只写实际用到的字节（不再整页 4 KB）。`bench3` 新输出的
`breakdown: flush/compact/merge/manifest` 四段可以直接看出写放大花在哪。

细节见 `docs/10-design/DESIGN3.md`。

## 动画讲解页

`fcs-anim.html` 是一个**单文件**的 React 动画页（11 章：页格式 → 写入/提交 → 清单 →
读路径 → 合并压实 → 空间回收 → 崩溃恢复 → 不挂死 → 坑清单 → 真字节 → 实测数据），
双击就能开，不需要服务器。

> 动画页是**上一批**做的（坑清单是当时的 36 条，DESIGN3 §12 现在已有 47 条）；
> 它讲的结构/字节格式没有变，只是没有本批新增的大值溢出页、范围墓碑和并发章节。

源码在 `web/`：

```bash
cd web && npm install && npm run dev     # 开发
cd web && npm run build                  # 重新生成 ../fcs-anim.html（单文件，~260 KB）
```

「真字节」那一章的数据由 `web/dump.cpp` 从一个真实的 `RamDev` 里 dump 出来
（超级块两槽 / 清单页 / 数据页的原始十六进制 + 解出的字段）。
