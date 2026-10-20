// 第 4 批：坑 85 ~ 91（第 9 轮 g7 并发 / P5 快照期收缩 → 第 10 轮「悬崖」与过期值）。
// 无历史头（第 10 轮的 backup/r10 只有部分）：老代码按 §12 复原（sketch）；新代码能从当前 include/fcs.h 原样引用的标 half。
// 数字来自 §12 / §13.9~13.10 当年实测，非本轮复测。

PIT({
  id: 85, round: '第 9 轮（g7 并发）', cat: '并发 / 锁纪律', found: 'g7 seed 1 首跑 + TSan', tags: ['scrub', 'mtx_', 'WGuard', 'TSan', '公开入口第一行持锁'],
  sym: '并发读者在时 `scrub` 的 `page()` / `ensure_ovfl_loc` 和读者的 `page()` 互相踢缓存 ⇒ **读者拿到别人的页**。单线程无感。',
  cause: '`scrub()` 公开入口不持 `mtx_`。同类：`get()` 在锁外取 `open_vers()`、`index_scan` 整个无锁。TSan 又抓 3 类：`wait_page` 锁外早退读 `inflight_` 要 acquire 原子读；`RamDev::np_` 要原子（异步写线程 truncate）；`Run::meta_pending` 读写竞争（改三态原子）。',
  rule: '**所有公开入口第一行持锁**（`WGuard` / 读锁）；跨线程读的标量一律原子。',
  fix: '`scrub()` = `WGuard g(this); if (g.busy) return RC::BUSY; return scrub_impl(...)`；其余同理。g7（含 TSan 版）固化进 `make test`。',
  verify: 'g7 全种子；TSan 0 报告（第 14 轮复核 16 次 0 报告）。',
  cost: '`scrub` 期间写者等待（第 13 轮改为观测锁 / 预算分片，坑 108）。',
  lesson: '"单线程无感"的并发 bug 只能靠并发测具 + TSan 常开抓。',
  rel: [29, 86, 108],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2668 `scrub()`；老代码按 §12 第 85 条复原',
    old: `- RC scrub(u32* bad, u32* first) { return scrub_impl(bad, first); }     // 不持锁：与读者互踢页缓存`,
    new: `  RC scrub(u32* bad = nullptr, u32* first = nullptr, u64 budget_bytes = 0) {
!     WGuard g(this); if (g.busy) return RC::BUSY;      // 公开入口第一行持锁
      return scrub_impl(bad, first, budget_bytes);
  }`
  },
  old: [
    { t: '读者线程 `page(9)` → 槽 1；同时 `scrub` 无锁 `page(13)` → 同一槽。', lanes: [{ n: '读者', ev: [{ a: 10, b: 40, l: 'page(9) 用指针', k: 'run' }] }, { n: 'scrub', ev: [{ a: 20, b: 35, l: 'page(13) 踢槽 1', k: 'io' }] }], now: 30, rows: [{ n: '缓存槽 1', items: ['s:页 9 → 页 13:bad'] }] },
    { t: '读者手里的指针已是页 13 的内容 ⇒ 读到别人的记录。', kv: { 'get': '错值 ✗', 'TSan': '数据竞争' }, ok: false },
  ],
  new: [
    { t: '`scrub` 持写锁 ⇒ 与读者串行；读者的页指针在其持锁期间稳定。', lanes: [{ n: '读者', ev: [{ a: 10, b: 40, l: '读锁 page(9)', k: 'lock' }] }, { n: 'scrub', ev: [{ a: 40, b: 70, l: 'WGuard scrub', k: 'lock' }] }], now: 40, kv: { 'get': '✓', 'TSan': '0 报告' }, ok: true },
  ],
});

PIT({
  id: 86, round: '第 9 轮（g7 并发）', cat: '并发 / 读写锁', found: 'g7 `3 6000 4`：15 分钟不结束、CPU 198%', tags: ['shared_mutex', '写者饥饿', 'XLock', 'wwait_', 'rwait_ 让一手'],
  sym: '4 读者 2 核：写者**永远**拿不到锁（15 分钟不结束）。修成写者优先后第二脚：写者满负荷时读者 10 s 只做了 **314 次** get。',
  cause: 'glibc 的 `std::shared_mutex` 默认读者优先，读者前赴后继 `lock_shared`。第二脚：写者优先 + 读者睡在 cv 上 ⇒ 写者 unlock → notify → 读者醒来要 50 µs，写者早又 lock 了。',
  rule: '自写 `XLock`：**有写者在等（`wwait_ > 0`）新读者退让**，自旋 64 次后睡 cv，最后一个读者出门叫醒写者；**让一手**：写者举牌前若有睡过的读者（`rwait_`）还没拿到共享锁，等它们进来（上界 200 µs）。',
  fix: '如右。',
  verify: 'g7 读者 get 314 → 12~19 万（2 读者）/ 85 万（4 读者），写者仍按时完成；`make test` g7 段加 `3 4000 4` 回归。',
  cost: '写极密时读者吞吐下降（取舍：嵌入式存储里"写必须推进"优先）；有读者等着时每次写入口多 ≤200 µs。',
  lesson: '标准库锁的公平性是平台实现细节；两个方向的饥饿都要有测试。',
  rel: [85, 108, 'r14-2'],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @518~548 `XLock::lock()`；老代码按 §12 第 86 条复原',
    old: `- std::shared_mutex mtx_;                 // glibc 读者优先 ⇒ 4 读者时写者饿死`,
    new: `  class XLock {
    void lock() {
      // 让一手：有读者还在睡（上一个写者刚走、它们没来得及醒），先等它们拿到共享锁（上界 200 µs）
!     if (rwait_.load(std::memory_order_seq_cst) > 0) {
          std::unique_lock<std::mutex> lk(mu_);
          cv_.wait_for(lk, std::chrono::microseconds(200), [&] { return rwait_.load(std::memory_order_seq_cst) == 0; });
      }
!     wwait_.fetch_add(1, std::memory_order_seq_cst);        // 先举牌：新读者看到就退让
      int e = 0;
      if (!st_.compare_exchange_strong(e, -1, std::memory_order_seq_cst)) { ... cv_.wait(...) ... }
      wwait_.fetch_sub(1, std::memory_order_seq_cst);
    }
  };`
  },
  old: [
    { t: '读者前赴后继 `lock_shared`；写者一直等。', lanes: [{ n: '读者 ×4', ev: [{ a: 0, b: 95, l: '共享锁从不空', k: 'lock' }] }, { n: '写者', ev: [{ a: 5, b: 95, l: '等待…', k: 'wait' }] }], now: 90, kv: { '写者': '饿死 ✗', 'CPU': '198%' }, ok: false },
    { t: '写者优先版第二脚：读者醒来 50 µs，写者又 lock 了。', lanes: [{ n: '写者', ev: [{ a: 0, b: 30, l: '写', k: 'lock' }, { a: 32, b: 62, l: '写', k: 'lock' }, { a: 64, b: 94, l: '写', k: 'lock' }] }, { n: '读者', ev: [{ a: 30, b: 32, l: '醒', k: 'wait' }, { a: 62, b: 64, l: '醒', k: 'wait' }] }], now: 94, kv: { '读者 10 s': '314 次 get ✗' }, ok: false },
  ],
  new: [
    { t: '`wwait_ > 0` ⇒ 新读者退让 ⇒ 写者拿到锁。', lanes: [{ n: '读者 ×4', ev: [{ a: 0, b: 30, l: '读', k: 'lock' }, { a: 45, b: 95, l: '读', k: 'lock' }] }, { n: '写者', ev: [{ a: 25, b: 30, l: '举牌', k: 'wait' }, { a: 30, b: 45, l: '写', k: 'ok' }] }], now: 45, kv: { '写者': '按时完成 ✓' } },
    { t: '让一手：`rwait_ > 0` 时写者先等读者进来（≤200 µs）。', lanes: [{ n: '写者', ev: [{ a: 0, b: 30, l: '写', k: 'lock' }, { a: 36, b: 60, l: '写', k: 'lock' }] }, { n: '读者', ev: [{ a: 30, b: 36, l: '进门读', k: 'ok' }, { a: 60, b: 66, l: '读', k: 'ok' }] }], now: 66, kv: { '读者 10 s': '12~85 万次 ✓' }, ok: true },
  ],
});

PIT({
  id: 87, round: '第 9 轮（P5）', cat: '空闲表重建 / 回退点', found: 'g5 seed 1 第 556 步 rollback(0) CORRUPT', tags: ['rebuild_free', 'deferred_', 'pending_free_', 'shrink', 'reload'],
  sym: '快照的清单页被当空闲页发给了新 run，回退点重放到一页数据 ⇒ `rollback(0)` CORRUPT。trace：`SNAP tail=16 man_last=6` → `FREE [6] deferred` → `SHRINK`（内含 `rebuild_free`）→ `RELOC ALLOC [6] user`。',
  cause: '以前只有 `rollback` 记得在 rebuild 之后 carve `deferred_` / `pending_free_`；`shrink` / `reload` 走到 `rebuild_free` 就漏。P5 放开快照期 shrink 后暴露。',
  rule: '**`rebuild_free` 自己把两张"还没归位"的表算作已用**（`deferred_` 是回退点还要用的页，`pending_free_` 要等提交点）。',
  fix: '如右；`rollback` 顺手简化（先决定 `deferred_` 再 rebuild）。也修了一个没被抓到的老洞：快照存活时提交失败 → `reload()` → deferred 页流入空闲表。',
  verify: 'g5 全种子（含快照期 shrink）。',
  cost: '无。',
  lesson: '"重建"函数必须自包含全部已用来源，不能依赖调用方事后打补丁。',
  rel: [33, 50, 74, 99],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @4167 `rebuild_free()`；老代码按 §12 第 87 条复原',
    old: `  void rebuild_free() { used = runs + 链 + 清单 + 内存链; free_ = 补集; }
- // rollback 之后：carve(deferred_); carve(pending_free_);      // 只有 rollback 记得；shrink / reload 漏`,
    new: `  void rebuild_free() {
      free_.clear(); man_hole_.clear();
      std::vector<std::pair<u32, u32>> used;
      for (auto& kv : runs_) { used.push_back({page0, run_block}); collect_ovfl(...); aux ... }
      ... 清单链 ...
      for_each_mem_chain([&](const OvflDesc& d) { used.push_back({d.pg0, d.npages}); });   // 坑 74
      // 坑 87：两张"还没归位"的表也算已用
!     for (auto& e : deferred_) used.push_back({e.second, e.first});
!     for (auto& e : pending_free_) used.push_back({e.second, e.first});
      free_ = 补集(used);
  }`
  },
  old: [
    { t: '快照 S 的清单页 6 在 `deferred_`；`shrink` 内 `rebuild_free` 只看 runs/清单 ⇒ 页 6 成空闲。', rows: [{ n: 'deferred_', items: ['d:页 6（S 的清单页）:pend'] }, { n: 'free_', items: ['f:页 6 ✗:bad'] }] },
    { t: '`RELOC ALLOC [6] user` ⇒ 页 6 被数据覆盖 ⇒ `rollback(0)` 读清单页读到数据 ⇒ CORRUPT。', kv: { 'rollback(0)': 'CORRUPT ✗' }, ok: false },
  ],
  new: [
    { t: '`rebuild_free` 把 deferred_/pending_free_ 算作已用 ⇒ 页 6 不进 free_。', rows: [{ n: 'deferred_', items: ['d:页 6:pend'] }, { n: 'free_', items: ['f:（不含 6）:free'] }], kv: { 'rollback(0)': '✓' }, ok: true },
  ],
});

PIT({
  id: 88, round: '第 10 轮（悬崖 ①）', cat: '性能 / 引用计数', found: 'gprof 两处各 19.6 s / 20 s', tags: ['count_refs', 'O(段²×单元)', 'Run::refs', '9 个落点'],
  sym: '每次提交 ~30 ms（1500 单元 × 12 段），5500 单元 × 93 段时 **1.5 s**；`compact_pass` 每次提交调两遍。',
  cause: '`Run::refs` 字段声明了却没人维护；取段集合 / 释放判定每次都遍历全部单元的全部段去数。',
  rule: '**`refs` 由每个插 / 删段落点 O(1) 维护**（`attach_run` 新插 `++`；`detach_seg` / retire / 归并摘段 / 压实摘段 / `M_SPLIT_DEL` / `segdels_` 回放 `--`；`split_pass` 父子同引用 `++`），`verify()` 用慢路 `count_refs` 交叉核对（"run refs"）。',
  fix: '`ref_inc` / `ref_dec` 接进 9 个落点。',
  verify: '悬崖第一层消失；`verify` "run refs" 0 不一致。',
  cost: '无。',
  lesson: '声明了但没人维护的字段比没有更危险——要么维护 + 交叉核对，要么删掉。',
  rel: [89, 90, 20],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3312 `ref_inc/ref_dec`、@3255 `count_refs`；老代码按 §12 第 88 条复原',
    old: `- u32 count_refs(u32 rid) { u32 n = 0; for (cells_) for (segs) if (seg.rid == rid) ++n; return n; }   // 每次 O(段 × 单元)，compact_pass 每提交调两遍`,
    new: `  // R0（第 10 轮）：run->refs = 全部单元里引用这条 run 的段数。段的每个插入/删除落点都维护 refs，
  // verify() 用慢路 count_refs 交叉核对。
! void ref_inc(u32 rid) { auto it = runs_.find(rid); if (it != runs_.end()) ++it->second->refs; }
! void ref_dec(u32 rid) { auto it = runs_.find(rid); if (it != runs_.end() && it->second->refs) --it->second->refs; }
  // 判定：if (r->refs == 0) 整条释放   （count_refs 只在 verify 里）`
  },
  old: [
    { t: '每次提交：`count_refs` 遍历 5500 单元 × 93 段 ×2。', kv: { '每次提交': '1.5 s ✗', 'gprof': '19.6 s + 20 s' }, ok: false },
  ],
  new: [
    { t: '`refs` O(1) 维护；判定 `refs == 0`。', kv: { '每次提交': 'ms 级 ✓', 'verify run refs': '0 不一致' }, ok: true },
  ],
});

PIT({
  id: 89, round: '第 10 轮（悬崖 ②）', cat: '层结构 / 上界', found: '20k 键 × 200 次提交：685 条 run，605 条在 lvl 5', tags: ['best_lvl + 1', 'LVL_MAX', 'level_pass', 'run 数 → 单元数'],
  sym: 'run 数趋向单元数（685 条 run、605 条在 lvl 5；320 次提交 851 条）：每条一张 bloom，`get` 逐条探，`compact_pass` 候选集膨胀。',
  cause: '压实产物层号 = `best_lvl + 1` 没有上界；`level_pass` 只看 `lvl < LVL_MAX(4)` 的层 ⇒ 产物压实两代就到 5/6/7 层，再没有任何合并路径接手。',
  rule: '**层号有上界且产物进 L0**（坑 90）；挂载时把老库的 5/6/7 层夹回 `LVL_MAX`（`apply_man`）；`verify()` 加 "run lvl" 检查。',
  fix: '如右。',
  verify: 'bench6 ovw run 数回到个位 / 十位；`verify` run lvl。',
  cost: '无。',
  lesson: '任何"+1"都要问上界在哪、越界后谁接手。',
  rel: [88, 90, 91],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @6478 `std::min(lvl + 1, LVL_MAX)`、@6707 `nr->lvl = 0`；老代码按 §12 第 89 条复原',
    old: `- nr->lvl = best_lvl + 1;          // 5/6/7 … level_pass 永远不看`,
    new: `  const u32 newlvl = std::min<u32>(lvl + 1, LVL_MAX);     // 层合并产物：夹到上界
  ...
! nr->lvl = 0;                     // 压实产物进 L0（seq 已由 finish_run 分配）
  // apply_man：老库 lvl > LVL_MAX 夹回 LVL_MAX；verify：\"run lvl\"`
  },
  old: [
    { t: '产物逐代升层，越过 LVL_MAX=4。', lvl: { L4: ['a:…:data'], L5: ['b:run ×605:bad'], L6: ['c:run ×40:bad'] }, kv: { 'run 数': '685 ✗' }, ok: false },
  ],
  new: [
    { t: '产物进 L0，层合并照常接手。', lvl: { L0: ['p:产物:good'], L1: ['m:合并产物:data'] }, kv: { 'run 数': '个位~十位 ✓' }, ok: true },
  ],
});

PIT({
  id: 90, round: '第 10 轮', cat: '读路径 / seq 全序', found: 'bench6 ovw 5000 500 200 200：老引擎 22% 键回读旧版本', tags: ['best_seq', '新鲜 seq', 'L0', '过期值'],
  sym: '**老代码同样有**：回读 22% 的键是旧版本（bad=1127）。',
  cause: '产物 O 取 `seq = best_seq`，最新源 F 还活着（别的单元仍引用）⇒ O 与 F 同 seq；O 进层合并后产物 M 的 seq = 组内最大 = F.seq，而 F 不在组内；M 的 rid 若比 F 大，对 F 仍最新的键，M 以组内老 run 的版本胜出。实例：cell[0,19) F(rid 116, seq …910, lvl 0) 持 tag 12857，M(rid 312, seq …910, lvl 1) 持 tag 12611，`seg_key` M > F ⇒ get 返回 12611。',
  rule: '**压实产物保留 `finish_run` 分配的新鲜 seq（全局最大），层号 0**。单元的段集合覆盖本单元所有键的全部已提交版本 ⇒ 产物里每个版本都是该键此刻最新的；之后刷盘的 run seq 更大、自然压过它。从此不存在同 seq 的两条 run。',
  fix: '`finish_run` 里 `r->seq = next_seq_++`；`compact_cell` 不再覆盖。老库遗留的同 seq 对在被合并前仍可能触发；升级后跑一次 `compact_file()` / `consolidate()` 清零。',
  verify: 'bench6 ovw bad 1127 → 0；g8 每次提交全量逐键比对。',
  cost: '无。',
  lesson: '"同 seq 看 rid"这种兜底比较一旦真的被用到，就说明身份已经不唯一了。',
  rel: [3, 9, 36, 89, 91],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @5219 `finish_run`、@6707；老代码按 §12 第 90 条复原',
    old: `- nr->seq = best_seq;                       // 与仍活着的最新源 F 同 seq
- nr->lvl = best_lvl + 1;`,
    new: `  // finish_run：
! r->seq = next_seq_++;                      // 新鲜 seq（全局最大）
  // compact_cell：
! nr->lvl = 0;                               // seq 已由 finish_run 分配（next_seq_++），不再覆盖`
  },
  old: [
    { t: 'O(seq 910) 与活着的 F(seq 910, rid 116) 同号。', lvl: { L0: ['F:F rid116 seq910 tag12857:data', 'O:O seq910:data'] } },
    { t: 'O 进层合并 ⇒ M(rid 312, seq 910, lvl 1) 持老版本 12611；`seg_key` M > F。', lvl: { L0: ['F:F rid116 seq910 tag12857:data'], L1: ['M:M rid312 seq910 tag12611:bad'] }, kv: { 'get': '12611 ✗（22% 键）' }, ok: false },
  ],
  new: [
    { t: '产物 O 取新鲜 seq 1000、进 L0。', lvl: { L0: ['O:O seq1000:new', 'F:F seq910:data'] } },
    { t: '之后任何合并产物 seq ≥ 1000 > F ⇒ 以最新版本胜出；不存在同 seq 对。', lvl: { L0: ['F:F seq910:data'], L1: ['M:M seq1000 tag12857:good'] }, kv: { 'get': '12857 ✓' }, ok: true },
  ],
});

PIT({
  id: 91, round: '第 10 轮（g8）', cat: '层合并 / 连续性', found: 'g8 关掉连续性检查后 3 个种子都在第 18 次提交失败', tags: ['snap_max 钉住', '组跨过中间 run', 'seq 连续', '提升到下一层'],
  sym: '第 5 次提交抓快照、第 17 次释放、第 18 次合并 ⇒ 读到过期值。',
  cause: '`level_pass` 跳过 `ver ≤ snap_max` 的 run，组从它后面开始：L0 = [P(9, 钉住), F10..F17] ⇒ 并出 M(17)；快照释放后 L0 = [P(9), F18..F25] 并出 M\'(25)，M\' 里 P 的老版本以 seq 25 压过 M(17) 里的新版本。',
  rule: '**合并组必须在全库 seq 顺序上连续**（组外不能有 seq 落在组内区间的 run）。显式检查：组在第一个夹缝处截断；若最老那条自己就被隔开，把它**提升**到下一层（只改内存层号，不入清单；安全性靠检查而不靠层号）。',
  fix: '`m = min(n, mcont)`；`m < 2` 且非单条重写 ⇒ `set[0]->lvl = lvl + 1` 重来。',
  verify: 'g8 每次提交全量逐键比对；`make test` g8 段。',
  cost: '无。',
  lesson: '`verify()` 只查结构账，查不出"读到旧版本"——正确性测试必须有逐键模型回读。',
  rel: [9, 48, 90, 69],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @6520~6540 `level_pass` 连续前缀；老代码按 §12 第 91 条复原',
    old: `- // level_pass：跳过 ver <= snap_max 的 run，从它后面开始取最老 n 条
- set = {F10..F17};  merge(set);           // 组跨过了被钉住的 P(9)`,
    new: `  size_t m = std::min(n, mcont);       // 连续前缀长度（组外没有 seq 落在区间内）
  if (m < 2) {
      if (n == 1 && dsingle) force1 = true;
      else {                           // 最老那条被组外 run 隔开：提升到下一层，重来
          if (lvl == LVL_MAX) continue;
!         set[0]->lvl = lvl + 1;       // 只改内存层号；安全性靠连续性检查
          did = true; continue;
      }
  }
  set.resize(m);`
  },
  old: [
    { t: '快照钉住 P(9)；组 {F10..F17} 跨过 P ⇒ M(17)。', lvl: { L0: ['P:P seq9 钉住:lock', 'F:F10..F17:data'], L1: [] } },
    { t: '释放后组 {P, F18..F25} ⇒ M\'(25)：P 的老版本以 seq 25 压过 M(17) 的新版本。', lvl: { L1: ['M:M seq17（新版本）:data', 'M2:M\' seq25（含 P 老版本）:bad'] }, kv: { 'get': '旧版本 ✗（第 18 次提交）' }, ok: false },
  ],
  new: [
    { t: '组在夹缝处截断：P 被隔开 ⇒ 提升到 L1（内存层号）。', lvl: { L0: ['F:F10..F17:data'], L1: ['P:P seq9 ↑:new'] } },
    { t: 'P 下次与 L1 的老 run 组队（连续）；不会再以更大 seq 压过新版本。', lvl: { L1: ['M:M seq17:good', 'P:P seq9 ✓:good'] }, kv: { 'g8 逐键比对': '✓' }, ok: true },
  ],
});
