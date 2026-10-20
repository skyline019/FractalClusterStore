# 第 15 次讨论：提交路径 fsync 出锁（先讨论语义，不动代码）

> 状态：**讨论稿，等拍板**。本轮（第 15 轮）没有改提交路径一行代码；本文的"预估"没有实测支撑，实测只有现状（§1）。
> 范式同 `DISCUSS12_R3.md`：先把语义、方案、坑、测试写清楚，用户拍板后单开实现。

## 0. 一句话

现在一次持久提交 `commit()` 在**排他锁内**做两次 `dev_->sync()`（屏障 → 写 SB → 屏障），读者在提交期间被挡住（ROUND14 §4.3 / §5.2 的"停顿地板 5~6 ms"）。
把 I/O 等待（wait_io + 两次 sync + SB 的 pwrite）挪到锁外，锁内只剩内存视图切换；**提交者自己仍然等到持久才返回**。
两个先说清楚的修正：
1. ROUND14 把它写成"语义变更（可见先于持久）"；细看读路径后的结论是：**读者的可见性模型其实不变**（§2），真正要改的是提交者的等待位置和两道内部纪律（页复用闸、SB 顺序）。
2. ROUND14 把 5~6 ms 整个归给两次 fsync，是没分解就下的结论。本轮用 `FCS_PROF` 分解了（§1）：**本沙箱**一次 fsync p50 0.25 ms，提交锁内时间的大头是 `flush_mem`（页编码 + 发写，avg 0.8~1.5 ms），fsync 段（`commit_delta`）avg 0.4~0.8 ms、尾巴 1~5 ms。所以：只搬 fsync 出锁，在本沙箱读者 max 大约 3.5 → ≈2.5 ms（[预估]）；要到亚毫秒得连 `flush_mem` 也出锁（§3.5 双内存表）。在 fsync 慢的设备上（消费级 SSD 1~5 ms、HDD 10~30 ms）fsync 才是大头，出锁的收益 = 2 × 设备 fsync 延迟。

## 1. 现状（实测 / 代码事实）

提交路径 `commit()` → `commit_locked(true)`（`WGuard` = XLock 排他，fcs.h L1648 / L1782）：

| 步骤 | 持锁？ | I/O | 备注 |
|---|---|---|---|
| `flush_mem`：内存表编码成 run 页、发写（async 时丢给 IO 线程） | 是 | 发写（不等） | CPU：2000 条 × 32 B 约几百 µs |
| `apply_ranges` / mode 0 的 rebalance+level_pass | 是 | — | mode 1/3 不在这里 |
| `wait_io`：等本批数据页落到设备 | 是 | **等** | 有界（TIMEOUT 闸） |
| `commit_delta`：写清单页、改内存清单、`write_sb` | 是 | 清单页写 + **sync → SB pwrite → sync** | 坑 81 的屏障协议 |
| `promote_pending`：上个提交点之前释放的页回 `free_`、tail_ 回落 | 是 | — | 闸：`sb_gen_ ≥ pend_need_gen_` |
| 之后 `maint_loop`（mode 1）：{拿锁 → 一个单元 → 放锁}，sync 只在循环**结束后锁外**做一次 | 单元持锁 | 锁外 sync | 第 12 轮 M2 已经是"锁外 sync"的样板 |

实测（ROUND14 §4.3，`stall` 探针，10 万键 × 100 B，写者定节拍 50 ms）：mode 1 读者 p99 20 µs、**单次 get max 5.7 ms**；mode 3 max 9.0 ms（那次与其它负载并行跑，偏高；本轮空机复跑 mode 1：**max 3.5 ms**、>1 ms 150 次 / 4 s）。

**本轮分解 [实测，2026-10-20，空机，`FCS_PROF=1`，sandbox 虚拟盘]**：

| 负载 | `flush_mem`（编码 + 发写） | `commit_delta`（清单页 + wait_io + sync → SB → sync） | `promote` |
|---|---|---|---|
| `stall 100000 100 4 0 0 1 -1 50`（2000 × 100 B / 批，io_threads 0） | avg **1.52** / max 2.55 ms | avg 0.42 / max 1.04 ms | 0.00 / 0.02 |
| `bench4 100000 2000 100 … ithr=0`（同批量，io_threads 0） | avg 0.83 / max 2.60 | avg 0.79 / max **5.48** | — |
| 同上 `ithr=1`（发写交给 IO 线程） | avg 0.77 / max 2.17 | avg 0.68 / max 3.30 | — |
| `hwmap 100000 2000 32 0 0`（2000 × 32 B / 批） | avg 0.49 / max 1.54 | avg 0.39 / max 1.63 | — |
| 单次 `FileDev::sync()`（1 页脏，300 次） | — | p50 **0.25** / p90 0.30 / p99 0.42 / max 0.66 ms | — |

读：本沙箱锁内时间 ≈ 2~3.5 ms/提交，其中 fsync 段 1/4 ~ 1/3，尾巴（max 5.48）是 fsync 排队。读者 max 3.5 ms ≈ flush_mem max + commit_delta max。`-DFCS_PROF` 的分解命令：`FCS_PROF=1 build/stall … | grep -a '\[prof\]' | awk '{a[$2]+=$3;n[$2]++;if($3>m[$2])m[$2]=$3} END{for(k in a)printf "%s avg=%.2f max=%.2f\n",k,a[k]/n[k],m[k]}'`（≈5 s）。

## 2. 语义：到底改了什么、没改什么

### 2.1 读者的可见性**本来就是"可见先于持久"**

`get()` 以 `open_vers() = vers_ + 1` 为视图（L944 / L1199），`get_at` 先查 `mem_`（`me->ver <= view`）——**未提交的 put 对所有线程的读者已经可见**（同一个 Store 没有事务隔离，排他锁只保证写者之间互斥）。
所以"另一个读者在提交返回前读到未落盘的数据"今天就是常态：它读的是 mem_。把 fsync 挪到锁外之后，读者在那个窗口里看到的是"run 已入账、mem_ 已清空"的状态，**同一份逻辑内容**，只是从内存表换到了 run。掉电后"读过的东西消失"这件事今天也一样会发生（mem_ 没落盘）。

结论：**读者可见性模型不变**，不需要"读者视图版本门"（ROUND14 §5.2 那个提法可以收回）。可选地暴露 `durable_vers()`，给真在意的应用自己判断（§4.4）。

### 2.2 保持不变的承诺

1. **`commit()` 返回 ⇒ 持久**：提交者在锁外等自己的 sync 完成才返回（不变）。
2. **崩溃一致性**：屏障协议不变（数据页 + 清单页先落 → SB → 再落），只是整个序列不再持排他锁。挂载永远回到某个完整 SB。
3. **快照 = 持久提交点**（坑 69）：`snapshot()` 必须等到它那次提交的 sync 完成（§4.3）。
4. **回退**（`rollback`）、**范围墓碑先于本批不落盘**（坑 78）：不动。

### 2.3 真正变化的两道内部纪律

1. **页复用闸**：现在 `free_pages` 记 `pend_need_gen_ = sb_gen_ + 1`，`promote_pending` 看 `sb_gen_ ≥ pend_need_gen_`；`sb_gen_` 在 `write_sb` 的第二次 sync 之后 +1（锁内，L3594）。出锁后，锁一放，别的线程（mode 3 后台维护、另一个提交者的 maint_loop）就能释放页；若它们记的是"在飞的那个 SB"的代数，等那个 SB 落盘它们就被放行——而它们的 DEL 并**不在**那个 SB 里 ⇒ 坑 99/104 同款（崩溃后清单仍引用被复用的页）。
   修法：拆成两个代数——`sb_built_`（锁内，SB 载荷抓取时 +1）和 `sb_synced_`（锁外 sync 完成后发布）；`free_pages` 记 `pend_need_gen_ = sb_built_ + 1`（= 下一个**还没抓载荷**的 SB），`promote_pending` 看 `sb_synced_ ≥ pend_need_gen_`。单线程时两者退化成今天的 `sb_gen_`。
2. **SB 顺序**：两个槽交替；提交者 A 的 SB 已 pwrite 未 sync 时，提交者 B 不能先写另一槽再 sync（B 的 sync 会把 A、B 一起落下去，这没问题；问题在 A 若失败回滚、B 却成功了——盘上会出现"B 完整、A 不完整"，挂载回到 B，而 B 的内存状态建立在 A 之上）。
   修法：**提交顺序器** `cmu_`（普通 mutex）：阶段二按进入顺序串行；A 失败 ⇒ 置 `dirty_`、B 在阶段二入口看到 `dirty_` 直接返回 IO（下一次公开调用 `ensure_ready` 按盘重建）。简单、正确、够用；真正的组提交（B 搭 A 的 sync）是 §4.5 的后话。

### 2.4 一类新的"读到随后被回滚的状态"

阶段二失败（sync/pwrite 报错、设备卡死 TIMEOUT）时，内存已经切到新视图而盘上还是旧提交点 ⇒ `dirty_` + `reload()` 回到盘上状态。中间窗口里读者读到的新 run 会"消失"。
今天也有同款：`commit_delta` 失败同样 `dirty_` 后重建、mem_ 的内容对读者可见过也会消失。**不是新语义，但窗口变宽了**（从 µs 到一次 fsync 的 ms 级），文档和 g6 要写明（§5）。

## 3. 方案：提交分两段

```
commit(durable=true)
  ├─ 阶段一（XLock 排他，只做内存 + 发写）
  │    ensure_ready / 组提交短路 / renumber（不变）
  │    flush_mem：编码页、发写（sync 设备：pwrite 进页缓存，不 fsync；async：丢 IO 线程）
  │    apply_ranges；(mode 0 的 rebalance/level 仍在这里——mode 0 不受益，接受)
  │    commit_delta 的内存部分 + 清单页发写 + **抓 SB 载荷到缓冲**（槽号、vers、inline 增量）
  │    sb_built_++；my_gen = wr_gen_；mem_.clear()；pend_delta_.clear()      ← 读者从此看到新视图
  │    （promote_pending **不在这里**：它要等 sb_synced_）
  ├─ 阶段二（放 XLock；拿 cmu_ 顺序器）
  │    wait_io（本批数据页 + 清单页）→ dev_->sync() → pwrite SB → dev_->sync()
  │    失败：dirty_ = true（下一次 ensure_ready 重建）；返回 IO/TIMEOUT
  └─ 阶段三（再拿 XLock，µs 级）
       sb_synced_ = 本次 built 号；durable_gen_ = my_gen；promote_pending()；（R7 的范围墓碑自动归一照旧）
  之后 maint_loop 不变（它本来就是锁外 sync）
```

要点：
- **锁内不再有任何等待 I/O 的点**（`wait_io` 也出去了）。锁内剩的是 `flush_mem`（编码本批页 + 发写）+ 改内存清单。按 §1 的分解，本沙箱地板 ≈ flush_mem 的 0.8~1.5 ms（max 2.6）[实测的锁内 CPU 段]，读者 max 预估 3.5 → ≈2.5 ms；fsync 慢的设备上收益按设备延迟 × 2 算。要再往下走看 §3.5。
- 维护单元的让路（第 14 轮 `maint_yield_now`，1 ms 时间片）不受影响：它们本来就在锁外 sync。
- `commit(false)`（逻辑提交 / 异步组提交）不变：它本来就不 sync。
- **持锁的调用方**（`maint_commit()`：compact_file / relocate / consolidate / checkpoint 自己就是提交点且持锁；`renumber_seqs_locked` 写全量清单 + SB；NOSPACE 重试里的中间提交点 `commit_delta(delta,false)`）走**内联版**：阶段二在锁内做（今天的行为）。它们是显式维护入口，停顿本来就不是承诺；留在锁内省掉一半的复杂度。
- `write_sb` 现在"内部先 wait_io"：拆成 `sb_capture()`（锁内）+ `sb_flush(buf)`（锁外）两半，内联版就是两半连着调。

### 3.5 第二步（可选）：`flush_mem` 也出锁 —— 双内存表
本沙箱的锁内大头是 `flush_mem`（§1）。经典做法：锁内把 `mem_` 换成只读的 `imm_`（指针交换，µs），新 put 进新的 `mem_`；锁外把 `imm_` 编码成 run 页并发写；完成后再拿锁把 run 入账、丢 `imm_`、走 §3 的阶段一后半（抓 SB 载荷）。
- 读路径：`get_at` 查 `mem_` → `imm_` → 段（多一次哈希查找；`imm_` 存在的时间 = 一次 flush）。可见性仍不变（imm_ 里的东西本来就可见）。
- 要碰的点：`job_overwrite` 钩子（第 14 轮作业增量账在 flush_mem 里按条看覆盖）要在锁外跑 ⇒ 改成对 `imm_` 的只读扫描 + 锁内一次性结算；NOSPACE 退路（flush_mem 里 retire + 中间提交点）要回到锁内；`commit(false)` 的 async 批；`mem_bytes` / 背压阈值按两表合计；`rollback` 期间不许有 `imm_` 在飞（等它）。
- 收益 [预估]：锁内只剩内存账（几十 µs ~ 几百 µs，大单元数时 `apply_ranges` / 清单内存更新）⇒ 读者 max 进入亚毫秒（维护单元时间片 1 ms 另算）。
- 代价：比 §3 大一圈（内存表两份、钩子重排），建议**先做 §3，量了再决定 §3.5**。

## 4. 逐个接口过一遍

### 4.1 多提交者
A 在阶段二、B 进阶段一：B 看到的是 A 切换后的内存视图（合法：它就是在 A 之后），B 抓自己的 SB 载荷（`sb_built_` = A+1），然后排队等 `cmu_`。A 成功 ⇒ B 做自己的 sync（也会顺带把 A 没等到的东西落了，无害）。A 失败 ⇒ `dirty_`，B 进阶段二先看 `dirty_` ⇒ 直接返回 IO，不写 SB。
**自动组提交**（`wr_gen_ == durable_gen_ && …` 短路）：`durable_gen_` 改到阶段三才推进 ⇒ A 在飞时，B 若没有新写也不会被短路放走（它必须等 A 持久）。要加一个"等在飞提交完成"的等待点：B 在阶段一发现 `commit_inflight_` 且自己无新写 ⇒ 放锁、等 A 的阶段三、返回 A 的结果。这就是最朴素的组提交（§4.5 之前的免费版）。

### 4.2 维护与页复用
- mode 1 的 `maint_loop` 在 `commit()` 末尾（阶段三之后）跑：不变。
- mode 3 后台线程：可能在 A 的阶段二期间拿锁干活、释放页 ⇒ 2.3(1) 的两个代数解决。它写的产物页 + 它自己的 sync 在锁外（不变）。
- `promote_pending` 的 tail_ 回落（第 14 轮）跟着挪到阶段三：**高水位回收策略（本轮的二步搬家）不受影响**——它只在 maint_loop 里跑。
- `deferred_`（快照钉住的页）/ `forget_snapshots`：不变。

### 4.3 快照 / 回退
`snapshot()` = `commit_locked(true)` + 抓状态（L2502）。改成：阶段一 → 放锁等阶段二 → 阶段三之后再抓（抓的是已持久的提交点）。`rollback(id)` 按老清单重放（不变）。
**快照存活期间的回收**（P5）不变。

### 4.4 可选 API：`durable_vers()`
返回 `sb_synced_` 对应的 `vers`。给"读到了必须保证持久"的应用（比如要把读到的值转发给第三方）一个自检手段。**不建议**做"只读已持久"的读模式：要么读 mem_ 也要挡（那就回到今天的全部语义都变），要么挡一半没意义。

### 4.5 真正的组提交（后话，先不做）
B 在 A 的阶段二期间到达：若 B 的数据页在 A 的第一次 sync **之前**已发写，A 的 sync 覆盖了它们，B 只需在 A 的 SB 之后写自己的 SB + 一次 sync（3 次 → 2 次 fsync 共用一次）。再进一步是"合并 SB"：B 不写 SB，A 的 SB 载荷里带上 B 的增量——但 A 的载荷在阶段一已经抓了，要改成"载荷在阶段二入口抓"。收益只在多提交者高并发时有（单提交者零收益），**本稿不含**。

## 5. 测试计划（实现轮）

| 段 | 内容 | 判据 |
|---|---|---|
| g16 split | 单线程：每次提交的 `[prof]` 分解；锁内段 < 1 ms（10 万键 × 100 B，2000 条/批） | 锁内时间不含任何 sync |
| stall（探针） | `stall 100000 100 4 {-1,0,20}`，mode 1 / 3 | 单次 get max 从 5.7 / 9.0 ms 降到 **< 1 ms**（无维护单元时）；p99 不升 |
| g7 多线程 | 写者满负荷 + 4 读者 + 2 提交者（新配置） | 无饿死、无 BUSY 风暴；TSan 0 报告 |
| g6 掉电（扩） | 在阶段二的每个 I/O 点注入崩溃（`FileDev` 钩子：丢弃未 sync 的写）；多提交者 + mode 3 + async | 挂载 OK 或回落到上一个完整 SB；**绝无**"SB 完整却引用了被复用的页"（页复用闸） |
| f2 快照 | snapshot 后立刻掉电 | 快照点必须在盘上 |
| 坑 99/104 回归 | NOSPACE 重试路径、shrink 的只改高水位 SB（`pend_delta_` 非空不 +1）| 两个代数下仍成立 |
| 现有 45(+g15) 段 + asan/TSan | 全绿 | — |

## 6. 代价与风险（明说）

- **复杂度**：提交路径多一个顺序器、两个代数、一个"等在飞提交"的等待点；`write_sb` 拆两半；`snapshot` / `maint_commit` / NOSPACE 重试 / renumber 四条持锁路径要逐个核对走的是内联版。预估 2~3 天 + 1 天测试。
- **失败窗口变宽**（§2.4）：不是新语义，但要写进 README 的"语义"一节。
- **mode 0 不受益**（维护在提交点之前、锁内）。
- **提交者延迟不变**（仍 2 次 fsync）；只有读者停顿受益。想要提交者也快 ⇒ §4.5 或 `commit(false)` + 定期 `commit(true)`（今天就有）。
- **本沙箱收益有限**（§1：fsync 只占锁内 1/4~1/3）：读者 max 3.5 → ≈2.5 ms [预估]；亚毫秒要 §3.5。真正的受益场景是 fsync 慢的设备。这一条是决定"做不做、先做哪个"的关键事实。
- **不做的替代方案**："NOMETASYNC 式单 fsync"（第 9 轮已否决，不重走）；"读者视图版本门"（§2.1 说明不需要）。

## 7. 请拍板的问题

1. 接受 §2 的语义结论（读者模型不变、新增的只是失败窗口变宽）？
2. 默认开还是开关？建议：**默认开，无开关**——它不改任何对外承诺；若要开关只为 A/B，用编译期宏 `FCS_COMMIT_SPLIT=0` 保留老路径一轮，下一轮删。
3. 要不要 `durable_vers()`（§4.4）？建议要（10 行）。
4. 组提交（§4.5）这轮不做，同意？
5. 范围：只做 §3（fsync 出锁，本沙箱收益 ≈1 ms、慢盘收益大），还是 §3 + §3.5（双内存表，读者 max 进亚毫秒，改动大一圈）？建议分两步、各自实测。
