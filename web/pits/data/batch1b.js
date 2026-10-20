// 第 1 批（b）：坑 102 ~ 109（第 12 ~ 13 轮）+ 第 14 轮的 7 条"假象 / 设计性发现"（未编号，id 'r14-1' …）。

PIT({
  id: 102, round: 12, cat: '边界 / 回绕', found: '第 12 轮全量评估的边界探针', tags: ['Vers', 'uint16_t', '回绕', '快照隔离'],
  sym: '第 65536 次逻辑提交之后：持有快照的读者看见"未来"的写（视图隔离破）；快照钉住所有新 run（8 → 29 条）；verify 报错。',
  cause: '内存里的逻辑时钟 `xdb::Vers = uint16_t`，每次 `commit(false)/commit()` ++ 且没有任何回绕处理；盘上（SB.vers / 清单 ver / 页头 ver）一直是 32 位。回绕后新写的 ver=0 比任何快照视图都"老" ⇒ 对所有视图可见。',
  rule: '**时钟类型在内存与盘上必须同宽**，而且要么够用一辈子（u32 ≈ 43 亿次提交），要么有显式的回绕协议。比较 ver 的地方不能默认"数值大 = 新"除非类型保证不回绕。',
  fix: '引擎内版本一律 `u32`（`vers_`、快照、清单条目、API 的 view 参数同步放宽）。',
  why: '不做 16 位回绕协议（序列号比较 + 窗口）：盘上本来就是 32 位，16 位只是一个历史别名；放宽类型是零风险方向。',
  verify: 'bench/probe 边界探针跨 65536 次提交：快照隔离正确；g11 回绕段。',
  cost: 'Snap 结构多 2 字节；无运行时开销。',
  lesson: '类型别名（`Vers`）掩盖了宽度；"够用吗"要用**最坏运行时间**算一遍：16 位 × 10 ms 提交 ≈ 11 分钟就回绕。',
  rel: [103, 69],
  code: { kind: 'real', src: '`backup/r12/fcs.h.pre-102` → `backup/r12/fcs.h.final-102`（`Store` 成员）',
    old: `  struct Snap {
-     Vers vers = 0;            // xdb::Vers = uint16_t
      u32 tail = 0, man_last = 0, nman = 0, man_first = 0;
      ...
  };
- Vers vers_ = 0;               // 每次逻辑提交 ++vers_，65536 次后回到 0
  u32 tail_ = 2;`,
    new: `  // 坑 102：这里以前是 xdb::Vers = uint16_t，每次逻辑提交 ++ 而没有任何回绕处理；
  //   第 65536 次 commit 之后 vers_ 回到 0。盘上一直是 32 位，只有内存里的时钟是 16 位
  //   ⇒ 跨回绕持有的快照看见"未来"的写、快照钉住所有新 run。修法：引擎内版本一律 u32。
  struct Snap {
!     u32 vers = 0;
      u32 tail = 0, man_last = 0, nman = 0, man_first = 0;
      ...
  };
! u32 vers_ = 0;
  u32 tail_ = 2;`
  },
  old: [
    { t: '提交到 v65534；读者在 v65535 建快照 S。规则：S 只看 `ver <= 65535` 的记录。', kv: { 'vers_ (u16)': 65534, '快照 S': 'view 65535' }, rows: [{ n: '记录', items: ['a:key 1 @v65534:data'] }] },
    { t: '再提交一次：`++vers_` 回绕到 **0**。新写的 key 1 新值带 ver=0。', kv: { 'vers_ (u16)': '0 ✗', '快照 S': 'view 65535' }, rows: [{ n: '记录', items: ['a:key 1 @v65534:data', 'b:key 1 @v0（其实是未来）:bad'] }] },
    { t: 'S 读 key 1：ver 0 ≤ 65535 ⇒ "可见" ⇒ 快照读到了**未来**的写。退休门禁 `ver > snap_max` 也被 0 骗过 ⇒ 新 run 全被钉住。', kv: { 'get(1, S)': '@v0 ✗（隔离破）', '被钉住的 run': '8 → 29 ✗' }, log: ['!probe: snapshot view 65535 sees ver 0 (future write)'], ok: false },
  ],
  new: [
    { t: '同左：v65534，快照 S@65535。', kv: { 'vers_ (u32)': 65534, '快照 S': 'view 65535' }, rows: [{ n: '记录', items: ['a:key 1 @v65534:data'] }] },
    { t: '再提交：`++vers_` = **65536**（u32 不回绕）。', kv: { 'vers_ (u32)': 65536, '快照 S': 'view 65535' }, rows: [{ n: '记录', items: ['a:key 1 @v65534:data', 'b:key 1 @v65536:new'] }] },
    { t: 'S 读 key 1：65536 > 65535 ⇒ 不可见，读到 v65534 ✓。u32 ≈ 43 亿次提交，10 ms 一次要 1.3 年。', kv: { 'get(1, S)': '@v65534 ✓', '被钉住的 run': '按规则' }, ok: true },
  ],
});

PIT({
  id: 103, round: 12, cat: '边界 / 回绕', found: '第 12 轮全量评估边界探针 `bench/probe/seqwrap.cpp`（3000 键里 2493 个旧值）', tags: ['seq', 'u32', 'SEQ_EXHAUSTED', 'RC::FULL', '重编号'],
  sym: 'run 序号 `seq` 回绕后：覆盖写**静默读到旧值**（3000 键里 2493 个），verify CORRUPT——比坑 102 更危险，因为没有任何报错。',
  cause: '`seq` 是 u32、只增不减、每次 flush / 产物各占一个。回绕后新 run 的 seq 最小 ⇒ 被当成最老 ⇒ 读路径的 `(seq, rid)` 全序把新值排在旧值后面。',
  rule: '**全序键不能静默回绕**：要么够用（u32 在 flush 频率下并不够——每次 flush 占一个，坑 102 的 u16 教训放大版），要么在到达前响亮失败 / 重编号。',
  fix: '第 12 轮：`SEQ_EXHAUSTED = 0xFFFFFF00`，`finish_run` 到线返回 `RC::FULL`（`commit()` 返回 FULL、批次仍在内存可 `abort`；维护路径返回 FULL 不动账）。第 13 轮 ①：提交点**重编号** `renumber_seqs_locked()`——按现有全序把 seq 压回 1..N，写全量清单快照，不改格式。',
  why: '不改 seq 为 u64（P2 格式升版）：盘上段身份 `(cell, rid, seq)` 固定 32 位。重编号在提交点做、一次 O(run 数)，频率极低（`seq_renum_at` 可调）。',
  verify: '`bench/probe/seqwrap.cpp 50 400`：RESULT OK（第 13 轮重编号后跨线继续写、读全对）；g13 `renumber` 段。',
  cost: '到线那一次提交多写一个全量清单快照（run 数 × 条目）。',
  lesson: '"静默错"比"崩"危险一个量级：边界探针要专门去找**不报错的错**。',
  rel: [102, 3, 4, 16],
  code: { kind: 'real', src: '`backup/r12/fcs.h.final-102` → `final-103`（FULL 线）；第 13 轮 `renumber_seqs_locked()`（`include/fcs.h`）',
    old: `  RC finish_run(RunWriter& w, ...) {
-     Run* r = new Run; r->seq = next_seq_++;     // u32 自增，回绕后 seq 最小 ⇒ 被当成最老
      ...
  }
  // 读路径：同键取 (seq, rid) 最大者 ⇒ 回绕后新值排在旧值后面 ⇒ 静默读到旧值`,
    new: `  static constexpr u32 SEQ_EXHAUSTED = 0xFFFFFF00u;   // 坑 103：序号用尽线
  RC finish_run(RunWriter& w, ...) {
!     if (next_seq_ >= SEQ_EXHAUSTED) { w.abandon(); delete r; return RC::FULL; }   // 响亮地失败
      Run* r = new Run; r->seq = next_seq_++;
      ...
  }
+ // 第 13 轮 ①：提交点重编号 —— 按现有 (seq, rid) 全序把 seq 压回 1..N，写全量清单快照（不改格式）
+ RC renumber_seqs_locked() {
+     std::vector<RunPtr> v = 按 seg_key 升序的全部 run;
+     u32 s = 1; for (RunPtr r : v) r->seq = s++;   // 段 map / cells_ 同步重建
+     next_seq_ = s; ++renumbers_;
+     return write_full_manifest();
+ }
+ // commit 路径：if (next_seq_ >= seq_renum_at_) renumber_seqs_locked();`
  },
  old: [
    { t: '`next_seq_` 逼近 2^32。键 k 的当前值在 run seq=4294967295。', rows: [{ n: 'runs_（seq）', items: ['a:seq 4294967290 {k=1}:data', 'b:seq 4294967295 {k=2}:data'] }], kv: { 'next_seq_': '0xFFFFFFFF', 'get(k)': '2 ✓' } },
    { t: '覆盖写 k=3，flush 出的新 run 拿到 **seq=0**（回绕）。', rows: [{ n: 'runs_（seq）', items: ['c:seq 0 {k=3}:bad', 'a:seq 4294967290 {k=1}:data', 'b:seq 4294967295 {k=2}:data'] }], kv: { 'next_seq_': '0 ✗' } },
    { t: '读 k：全序取 seq 最大 ⇒ seq=4294967295 的旧值 2 胜出。**没有任何报错**。', rows: [{ n: 'runs_（seq）', items: ['c:seq 0 {k=3} 被当最老:bad', 'b:seq 4294967295 {k=2} 赢:data'] }], kv: { 'get(k)': '2 ✗（应为 3）', 'seqwrap 探针': '2493/3000 旧值 ✗' }, log: ['!verify: CORRUPT (newest value not visible)'], ok: false },
  ],
  new: [
    { t: '第 12 轮：到 `SEQ_EXHAUSTED` 线 `finish_run` 返回 **FULL**——批次仍在内存，可 abort；不再静默。', rows: [{ n: 'runs_（seq）', items: ['a:seq 4294967290:data', 'b:seq 4294967295:data'] }], kv: { 'commit()': 'RC::FULL（响亮）' } },
    { t: '第 13 轮：到 `seq_renum_at` 线在提交点**重编号**：按现有全序把 seq 压回 1..N，写全量清单快照。', rows: [{ n: 'runs_（seq）', items: ['a:seq 1 {k=1}:new', 'b:seq 2 {k=2}:new'] }], kv: { 'next_seq_': 3, 'renumbers_': 1 } },
    { t: '覆盖写 k=3 ⇒ seq 3，全序正确。`seqwrap 50 400`：RESULT OK。', rows: [{ n: 'runs_（seq）', items: ['a:seq 1 {k=1}:data', 'b:seq 2 {k=2}:data', 'c:seq 3 {k=3}:good'] }], kv: { 'get(k)': '3 ✓' }, ok: true },
  ],
});

PIT({
  id: 104, round: 13, cat: '提交点 / 崩溃恢复', found: 'g6 seed 2 掉电注入（第 12 轮的头就有）', tags: ['shrink', 'pend_delta_', 'pending_free_', 'write_sb', 'sb_gen_', '提交点'],
  sym: '掉电重开后，一个**已经提交**的键 NOTFOUND。trace：`FREE [104] safe → SHRINK 105→104 → COMMIT(不含 DEL) → 掉电 → MOUNT → REPLAY rid=104`——挂到的提交点引用着设备之外的页。',
  cause: '提交点之外做的维护（mode 1 提交后 / `maintain()` / mode 3 线程）把源 run 的 DEL 留在 `pend_delta_`、页留在 `pending_free_`（坑 99 的闸关着），**盘上的 SB 还引用这些 run**。`shrink()` 只看 `live_top()`（内存里已没有这些 run）就截断，再 `++vers_; write_sb()` 写一个不含 DEL 的 SB（shrink 不是提交点——g5 明确依赖"shrink 之后重开 == 上一个提交点"）。第二张脸：这次 `write_sb` 还 `++sb_gen_`，下一次 `promote_pending` 把盘上仍被引用的页放行复用。',
  rule: '**放行 `pending_free_` / 下调高水位，前提都是"盘上 SB 不再引用这些页"，而这只有折叠了 `pend_delta_` 的 SB 才成立。**"只写 SB" ≠ "提交点"。',
  fix: '两处：`shrink()` 的活区算上 `pending_free_`（闸没开 = 盘上可能还引用）；`write_sb()` 只在 `pend_delta_` 为空时 `++sb_gen_`。',
  why: '不让 shrink 顺手折叠 `pend_delta_` 成为提交点：那会改变 shrink 的语义（g5 的回退点依赖），而且 mode 3 线程持有的作业状态也不允许随手提交。',
  verify: 'g6 seed 2 及全部种子；g13 `shrink after maintain` 段；FCS_TRACE 对账。',
  cost: 'shrink 在维护刚释放一批页、还没过提交点时收不到最低（下一个提交点之后再 shrink 即可）。',
  lesson: '两个"看起来一样"的动作（写 SB / 提交点）必须在代码里有不同的名字和不同的后置条件。',
  rel: [99, 105, 84, 82],
  code: { kind: 'real', src: '`backup/r13/fcs.h.pre-r13` → `backup/r13/fcs.h.final`，`shrink()` 与 `write_sb()`',
    old: `  RC shrink() {
      ...
      promote_pending();
!     u32 top = live_top();                  // 只看内存里的 run：刚被维护摘掉的源 run 不算活区
      ...
      tail_ = top; rebuild_free();
      dev_->truncate(top);                   // 盘上 SB 仍引用的页被截掉
!     ++vers_; return write_sb();            // 这个 SB 不含 pend_delta_ 里的 DEL
  }
  RC write_sb() {
      ... dev_->sync();
!     ++sb_gen_;                             // 任何 SB 写入都算"提交点已落盘"⇒ 下次 promote_pending 放行
      return RC::OK;
  }`,
    new: `  RC shrink() {
      ...
      promote_pending();
      u32 top = live_top();
+     // 坑 104：没过提交点的待释放页一律当活区（盘上 SB 可能还引用它们）
!     for (auto& d : pending_free_) top = std::max(top, d.second + d.first);
      if (top > tail_) top = tail_;
      ...
      tail_ = top; rebuild_free();
      if (dev_->truncate((PageId)top)) dev_pages_ = top;
      ++vers_; return write_sb();
  }
  RC write_sb() {
      ... dev_->sync();
+     // 坑 104：pend_delta_ 非空（shrink 这种只改高水位的 SB）⇒ 这次 SB 没把提交点之外维护的 DEL 落盘，
+     // 不能当作"提交点已落盘"放行 pending_free_
!     if (pend_delta_.empty()) ++sb_gen_;
      return RC::OK;
  }`
  },
  old: [
    { t: '盘上 SB(v7) 引用 run **r104**（页 100~104）。mode 1 提交后维护把 r104 重写成低处的 **r105**（页 10~14）：内存里 r104 已删，DEL 留在 `pend_delta_`，页留在 `pending_free_`（坑 99 的闸关着）。', rows: [{ n: '盘上 SB(v7) 引用', items: ['r104:r104 页100-104:data'] }, { n: '内存 runs_', items: ['r105:r105 页10-14:new'] }, { n: 'pend_delta_ / pending_free_', items: ['d:DEL r104:tomb', 'pf:页100-104:pend'] }], kv: { 'sb_gen_': 7 } },
    { t: '应用调 `shrink()`：`live_top()` 只看内存里的 run ⇒ 活区顶 = 15。可是**盘上的 SB 仍引用 100~104**（DEL 还没落盘）。', rows: [{ n: '盘上 SB(v7) 引用', items: ['r104:r104 页100-104:data'] }, { n: '内存 runs_', items: ['r105:r105 页10-14:new'] }, { n: 'pending_free_（闸关）', items: ['pf:页100-104:pend'] }], kv: { 'live_top()': '15', '盘上仍引用 100-104？': '是' } },
    { t: '`truncate(15)`：页 100~104 被截掉。再 `write_sb()`：这个 SB 只改高水位，**不含 DEL r104**（pend_delta_ 不折叠），却 `++sb_gen_`。', rows: [{ n: '盘上 SB(v8) 引用', items: ['r104:r104 页100-104（设备之外！）:bad'] }, { n: '设备', items: ['dev:0 … 15:data', 'cut:100-104 已截:out'] }], kv: { 'sb_gen_': '8 ✗', 'SB 含 DEL r104？': '否' }, ok: false },
    { t: '掉电 → 重开挂 SB(v8)：REPLAY rid=104 去读页 100 ⇒ 设备之外 ⇒ 已提交的键 NOTFOUND。第二张脸：即使不掉电，`sb_gen_` 已前进 ⇒ 下次 `promote_pending` 把 100~104 放行复用。', rows: [{ n: '重开 REPLAY', items: ['r104:rid=104 → 页 100 ?:bad'] }], kv: { 'get(已提交键)': 'NOTFOUND ✗' }, log: ['!FREE [104] safe → SHRINK 105→104 → COMMIT(no DEL) → crash → MOUNT → REPLAY rid=104'], ok: false },
  ],
  new: [
    { t: '同左：维护把 r104 搬成低处的 r105，DEL 在 `pend_delta_`、页在 `pending_free_`。', rows: [{ n: '盘上 SB(v7) 引用', items: ['r104:r104 页100-104:data'] }, { n: '内存 runs_', items: ['r105:r105 页10-14:new'] }, { n: 'pend_delta_ / pending_free_', items: ['d:DEL r104:tomb', 'pf:页100-104:pend'] }], kv: { 'sb_gen_': 7 } },
    { t: '`shrink()`：活区顶 = max(live_top, **pending_free_ 的顶**) = 105 ⇒ 不截 100~104（闸没开 = 盘上可能还引用）。', rows: [{ n: '盘上 SB(v7) 引用', items: ['r104:r104 页100-104:data'] }, { n: '设备', items: ['dev:0 … 105 保留:data'] }], kv: { 'top': '105 ✓', 'truncate': '不动 100-104' } },
    { t: '`write_sb()`：`pend_delta_` 非空 ⇒ **不** `++sb_gen_`。这个 SB 只是高水位记录，不是提交点。', kv: { 'sb_gen_': '7 ✓（不变）', 'pend_need_gen_': 8 }, rows: [{ n: '盘上 SB(v8)', items: ['sb:SB v8（无 DEL，不算提交点）:sb'] }], ok: true },
    { t: '掉电重开：SB(v8) 引用的 r104 页仍在设备里 ⇒ 读得到。下一个真正的提交点折叠 DEL 后 `sb_gen_=8`，100~104 才放行。', rows: [{ n: '重开 REPLAY', items: ['r104:rid=104 → 页 100 ✓:good'] }], kv: { 'get(已提交键)': '找到 ✓' }, log: ['+g6 seed 2: PASS; FCS_TRACE 对账 0 mismatch'], ok: true },
  ],
});

PIT({
  id: 105, round: 13, cat: '挂载 / 会话状态', found: 'q1 async2（第 13 轮 cheap 规则让它合并了）', tags: ['open()', 'reload()', 'pend_delta_', 'gpend_', 'owner beyond tail'],
  sym: '同一个 `Store` 对象 `close` 后再 `open()` 同一设备：`verify` 报 `owner beyond tail`；`compact_file` 后 400 行只剩 240。',
  cause: '`clear_state()` 故意**不清** `pend_delta_`——同会话 IO 错误后的 `reload()` 需要把后台已落页的 run 重新挂上。但 `open()` 是新挂载：上一会话没过提交点的合并产物（分配在 SB 高水位之上）被重新挂上、源被摘掉，而高水位按 SB 回退 ⇒ 产物页在 `tail_` 之上 ⇒ 被当空闲页分出去。',
  rule: '**挂载状态 = 盘上 SB 的闭包，不多不少。**会话内的"在飞"状态（pend_delta_ / gpend_）只在同会话 `reload()` 有意义；跨 `open()` 必须清零。若保留，`tail_` 必须覆盖它们。',
  fix: '`open()` 清 `pend_delta_` / `gpend_`；`reload()` 若 `pend_delta_` 非空，把 `tail_` 抬到这些 run 之上。',
  why: '不改成"open 也重新挂上在飞产物"：它们没过提交点，按定义不属于任何提交点，重新挂上等于凭空多一个提交点。',
  verify: 'q1 async2；g13 `reopen` 段；第 12 轮没撞上是因为 q1 的 20 条单页 run 不触发合并。',
  cost: '无。',
  lesson: '一个成员"故意不清"要写明**对哪条路径**有意义；新路径复用同一个清理函数时要重新审视每个"故意"。',
  rel: [104, 25, 14],
  code: { kind: 'real', src: '`backup/r13/fcs.h.pre-r13` → `backup/r13/fcs.h.final`，`open()` / `reload()`',
    old: `  RC open(BlockDev* dev, u8* arena, u32 arena_pages) {
      clear_state();                 // 故意不清 pend_delta_（reload 要用）
!     // pend_delta_ 里上一会话没过提交点的产物被当成"后台已落页的 run"重新挂上
      ... 读 SB → 重放清单 → tail_ = SB.tail（低于那些产物）...
  }`,
    new: `  RC open(BlockDev* dev, u8* arena, u32 arena_pages) {
      clear_state();
!     pend_delta_.clear(); gpend_.clear();   // 坑 105：新挂载 = 盘上 SB 的闭包；上一会话的在飞产物不算
      ... 读 SB → 重放清单 → tail_ = SB.tail ...
  }
  RC reload() {                              // 同会话 IO 错误后的重载：在飞产物保留
      ...
+     for (auto& e : pend_delta_) if (e.add) tail_ = std::max(tail_, e.page0 + e.npages);   // 抬到它们之上
  }`
  },
  old: [
    { t: '会话 1：SB 高水位 tail=300。提交后维护合出产物 P（页 300~340），DEL 源 / ADD P 都在 `pend_delta_`（没过提交点）。', rows: [{ n: '盘上 SB', items: ['s:源 run 页 100-180:data', 'sb:tail=300:sb'] }, { n: 'pend_delta_（在飞）', items: ['P:ADD P 页300-340:new', 'd:DEL 源:tomb'] }], kv: { 'tail_（内存）': 341 } },
    { t: '`close()`（`clear_state` 故意保留 `pend_delta_`）→ 同一对象 `open()`：按 SB 重放 ⇒ `tail_=300`；然后 `pend_delta_` 被当成"后台已落页的 run"**重新挂上** P、摘掉源。', rows: [{ n: '内存 runs_', items: ['P:P 页300-340（在 tail 之上！）:bad'] }, { n: '高水位', items: ['sb:tail_=300:sb'] }], kv: { 'P.page0 ≥ tail_': '是 ✗' } },
    { t: '页 300 之上按定义是空闲 ⇒ 下一次 flush 把新 run 分配到 300~… ⇒ 覆盖 P。verify：`owner beyond tail`；`compact_file` 后 400 行剩 240。', rows: [{ n: '设备 300-340', items: ['x:新 run 覆盖 P:bad'] }], kv: { 'verify': 'owner beyond tail ✗', '行数': '400 → 240 ✗' }, log: ['!q1 async2: verify owner beyond tail'], ok: false },
  ],
  new: [
    { t: '同左会话 1。', rows: [{ n: '盘上 SB', items: ['s:源 run 页 100-180:data', 'sb:tail=300:sb'] }, { n: 'pend_delta_（在飞）', items: ['P:ADD P 页300-340:new', 'd:DEL 源:tomb'] }], kv: { 'tail_（内存）': 341 } },
    { t: '`open()` 先清 `pend_delta_ / gpend_`：挂载状态 = SB 闭包。源 run 仍在（它过了提交点），P 不存在。', rows: [{ n: '内存 runs_', items: ['s:源 run 页 100-180:data'] }, { n: '高水位', items: ['sb:tail_=300:sb'] }, { n: 'pend_delta_', items: ['e:∅:hole'] }], kv: { '状态': 'SB 闭包 ✓' }, ok: true },
    { t: '同会话 `reload()`（IO 错误恢复）仍保留在飞产物，但把 `tail_` 抬到它们之上 ⇒ 不会被当空闲页。', rows: [{ n: 'reload 后', items: ['P:P 页300-340:new', 'sb:tail_=341:sb'] }], kv: { 'verify': 'OK ✓' }, ok: true },
  ],
});

PIT({
  id: 106, round: 13, cat: '存活账 / 层合并触发', found: '第 13 轮 ③⑤ 层合并规则验收', tags: ['live_bytes', 'settle_ranges', 'del_range', '脏度', 'seg_totals'],
  sym: '`del_range` 删掉半个库之后，承载这些键的 run **脏度仍是 0**：层合并的 (d)(e) 脏度规则对它全盲，空间只能靠单元压实按预算慢慢啃。',
  cause: 'run 级 `live_bytes` 由 flush 的 `probe_prev`、归并产物、压实搬运维护；`settle_ranges` 只结**单元级** live/dead，从不碰 run 级 `live_bytes`。于是层合并眼里"被整段删掉的 run"还是满活的。',
  rule: '**触发合并的脏度估计量必须对每一种"变死"的途径都响应**（覆盖、点删、范围删）。`live_bytes` 允许高估（最多多并一次），不允许系统性漏掉一类垃圾。',
  fix: '`settle_ranges` 按"段字节 × 活条数占比"（`seg_totals` memo，至多读两张边界页）估计墓碑前后的活字节差，从 run 的 `live_bytes` 扣；惰性账不碰（补算时 `lb_acc` 自然按墓碑后的赢家算）。',
  why: '不精确重算（逐页扫）：`del_range` 要保持 O(墓碑) 代价；估计量够用因为 `live_bytes` 本就是只会高估的触发量。',
  verify: 'g13 `merge` 段：`del_range` 半库后下一轮维护即合并，空间回收；ROUND13 §4 bench3r 写放大未上升。',
  cost: '`del_range` 每条墓碑多读 ≤2 张边界页（估占比）。',
  lesson: '每加一种删除途径，都要列一遍"哪些估计量 / 触发器应该看见它"。',
  rel: [63, 60, 27, 107],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `settle_ranges()`（第 13 轮）；老代码按 pre-r13 的行为复原',
    old: `  void settle_ranges(Delta& d) {
      for (每个被墓碑触及的单元 c) {
          ... 结单元级 live/dead（c->live -= 被盖字节; c->dead += ...）...
!         // run 级 live_bytes 不动 ⇒ 层合并脏度 = 0
      }
  }`,
    new: `  void settle_ranges(Delta& d) {
      for (每个被墓碑触及的单元 c) {
          ... 结单元级 live/dead ...
+         // 坑 106：范围墓碑也要扣 run 级 live_bytes，否则层合并对 del_range 全盲。
+         // 估计：段字节 × 活条数占比（seg_totals memo，至多读两张边界页）
+         for (auto& sg : c->segs 与墓碑相交) {
+             u64 bytes = seg_bytes_est(sg) * covered_frac;
!             if (d.r->live_bytes >= bytes) d.r->live_bytes -= bytes; else d.r->live_bytes = 0;
+         }
      }
  }`
  },
  old: [
    { t: 'run R：100 KB，全活。层合并脏度规则：dirty% = 1 − live_bytes / bytes。', lvl: { L2: ['R:R 100 KB live=100 KB:data'] }, kv: { 'R.live_bytes': '100 KB', 'dirty%': '0%' } },
    { t: '`del_range` 盖住 R 60% 的键。单元级账：live 降、dead 升。**run 级 `live_bytes` 没动**。', lvl: { L2: ['R:R 100 KB（60% 被盖）:data'] }, rows: [{ n: 'ranges_', items: ['G:G 盖 60%:tomb'] }], kv: { '单元 live': '40 KB', 'R.live_bytes': '100 KB ✗', 'dirty%': '0% ✗' } },
    { t: '层合并看 R：dirty 0% ⇒ 不并。60 KB 垃圾只能靠单元压实按预算一个单元一个单元啃。', lvl: { L2: ['R:R 60 KB 垃圾滞留:bad'] }, kv: { '合并触发': '否 ✗', '回收速度': '单元压实预算' }, ok: false },
  ],
  new: [
    { t: '同左：R 100 KB 全活。', lvl: { L2: ['R:R 100 KB live=100 KB:data'] }, kv: { 'R.live_bytes': '100 KB', 'dirty%': '0%' } },
    { t: '`del_range` 盖 60%：`settle_ranges` 用 `seg_totals`（≤2 张边界页）估"段字节 × 被盖占比" ⇒ 从 `live_bytes` 扣 60 KB。', lvl: { L2: ['R:R 100 KB live≈40 KB:data'] }, rows: [{ n: 'ranges_', items: ['G:G 盖 60%:tomb'] }], kv: { 'R.live_bytes': '≈40 KB ✓', 'dirty%': '≈60%' } },
    { t: '下一轮维护：dirty 60% ≥ 阈值 ⇒ (d) 规则合并 R ⇒ 产物 40 KB，60 KB 回收。估多了最多多并一次。', lvl: { L2: ['P:P 40 KB:good'] }, kv: { '合并触发': '是 ✓', '回收': '60 KB' }, ok: true },
  ],
});

PIT({
  id: 107, round: 13, cat: '归并丢弃规则 / 写放大', found: '第 13 轮 (d)(e) 规则验收（顶层 422 页反复重写）', tags: ['merge_run_set', 'newer_has', 'newer_may_contain', 'shadow_dropped', '组外更新'],
  sym: '按脏度触发的合并**白写**：顶层 run 422 页重写后还是 422 页，下一次维护再写一遍——写放大上升、空间不回收。',
  cause: '归并只丢三类：组内被盖副本、可丢墓碑、范围墓碑盖住的记录。被**组外更新 run**（seq 严格大于组内最大 seq）盖住的记录照搬进产物——而脏度正是由这些组外更新造成的，所以"脏"的 run 合并后一点没变干净。',
  rule: '**合并产物不应包含任何对读路径永远不可见的记录。**被更新 run 盖住的键，读路径永远先命中更新 run ⇒ 老值可以物理丢弃。安全性：组只含 `ver > snap_max` 的 run，没有快照看得见它们；盖住它的更新 run 在同一个 SB 里（或更早）；回退到更早回退点时产物和盖住它的 run 一起消失。',
  fix: '回调里 `newer_may_contain(k, maxseq)`（bloom）命中才 `newer_has(k, maxseq, …)` 精确确认，确认了跳过（`++shadow_dropped_`）；对墓碑探测返回非 OK 时不丢（安全不回收）。`page()` 指针即取即用的老约束在这里成立：`Cur::buf` 是私有拷贝、`merge_emit` 的描述符已拷出，回调内可安全探测别的 run。',
  why: '不只靠 bloom：误判会丢真活的记录，必须精确确认；不精确扫全部更新 run：bloom 先筛，精确只在命中时做。',
  verify: 'ROUND13 §4：顶层 422 页重写后缩到实际存活；`shadow_dropped()` 计数；bench3r 写放大 `200000 2000` 3.48×（含第 12 轮单条重写取舍）。',
  cost: '每条候选记录一次 bloom（命中再一次点查更新 run）。',
  lesson: '触发条件（脏度）和执行动作（丢弃规则）必须对同一类垃圾说同一句话，否则触发器会空转。',
  rel: [94, 106, 34],
  code: { kind: 'real', src: '`backup/r13/fcs.h.pre-r13` → `backup/r13/fcs.h.final`，`merge_run_set` 回调',
    old: `  auto emit = [&](u64 k, const u8* p, u16 l, const u8* dsc, RunPtr sr) {
      if (l == 0 && !dsc && !older_may_contain(k, minkey)) { ...; return true; }   // 坑 94
!     // 被组外更新 run 盖住的记录：原样搬走（只在账上记成"被盖住"）
      bool added = dsc ? w.add_take(k, v, dsc) : w.add(k, v, p, l);
      ...
  };   // 顶层两条 run 81% 脏长期不动：422 页 → 422 页`,
    new: `  auto emit = [&](u64 k, const u8* p, u16 l, const u8* dsc, RunPtr sr) {
      if (l == 0 && !dsc && !older_may_contain(k, minkey)) { ...; return true; }
+     // 第 13 轮：被**组外更新的 run**（seq 严格大于组内最大 seq）盖住的记录不再搬进产物。
+     // 安全性：组只含 ver > snap_max 的 run；游标页是私有拷贝，探测不会踢翻它们。
!     if (newer_may_contain(k, maxseq, 0) && newer_has(k, maxseq, 0xFFFFFFFFu)) { ++dropped_shadowed; return true; }
      bool added = dsc ? w.add_take(k, v, dsc) : w.add(k, v, p, l);
      ...
      lb += osz;    // 走到这里 = 没有更新的 run 盖住它 ⇒ 产物 live_bytes 精确
  };`
  },
  old: [
    { t: '顶层 R3（422 页）；L0 的 R9 更新了 R3 里 81% 的键。脏度 81% ⇒ 触发 (d) 规则重写 R3。', lvl: { L0: ['R9:R9 更新 81% 键:data'], L3: ['R3:R3 422 页 dirty 81%:data'] }, kv: { '触发': '(d) 脏度 ≥ 阈值' } },
    { t: '单源重写 R3：被 R9 盖住的记录不在丢弃规则里 ⇒ **原样搬进**产物 P。', lvl: { L0: ['R9:R9:data'], L3: ['P:P 422 页（还是 81% 脏）:bad'] }, kv: { '产物页数': '422 ✗', 'dirty%': '81%' } },
    { t: '下一次维护：P 仍 81% 脏 ⇒ 再写一遍 422 页。空转循环。', lvl: { L0: ['R9:R9:data'], L3: ['P2:P2 422 页:bad'] }, kv: { '每轮白写': '422 页 ✗' }, log: ['!EVAL12 §6 #5: top level 2 runs 81% dirty never shrink'], ok: false },
  ],
  new: [
    { t: '同左：R3 81% 脏，触发重写。', lvl: { L0: ['R9:R9 更新 81% 键:data'], L3: ['R3:R3 422 页 dirty 81%:data'] }, kv: { 'maxseq（组内）': 'R3.seq' } },
    { t: '回调：每条记录先 bloom `newer_may_contain(k)`，命中再 `newer_has(k, maxseq)` 精确查 R9 ⇒ 被盖住的 81% **跳过**（`shadow_dropped`）。', lvl: { L0: ['R9:R9:data'], L3: ['P:P 80 页:new'] }, kv: { '产物页数': '≈80 ✓', 'shadow_dropped': '≈81% 条' } },
    { t: '产物 `live_bytes` 精确（走到 add 的都没被盖）⇒ 脏度归零，不再空转。墓碑探测返回非 OK 时不丢（安全不回收）。', lvl: { L0: ['R9:R9:data'], L3: ['P:P 80 页 dirty 0%:good'] }, kv: { 'dirty%': '0% ✓', '下一轮': '不触发' }, ok: true },
  ],
});

PIT({
  id: 108, round: 13, cat: '分步作业 / 空间所有权', found: '第 13 轮流式产物验收（g11 没撞上：源 < 512 页不预留）', tags: ['流式预留', 'owner 8', 'verify page unowned', 'shrink', 'abandon_job'],
  sym: '`FCS_MERGE_BUDGET_DEFAULT=256` 之后 mode 1 默认就会开分步作业：`verify` 报 `page unowned`；`shrink()` 之后作业**写到设备之外**。',
  cause: '作业的 `RunWriter` 流式预留了一段连续页：既不在清单里（产物还没落地）也不在 `free_` 里（已被占）⇒ verify 眼里无主。`shrink()` 按 `live_top()` 截断时不知道预留区的存在 ⇒ 截掉它，作业下一片继续往里写。',
  rule: '**每一页在任何时刻都必须有且只有一个所有者**（清单 / free_ / pending_free_ / 作业预留），而且所有改变高水位的操作都必须认识全部所有者类型。',
  fix: 'verify 认 owner 8（作业预留）；`shrink()` 开头 `abandon_job()`（预留归还、作业从头再来——第 14 轮起预留在活区之下时也先放弃，简单优先）。',
  why: '不让 shrink 绕过预留区继续截：预留可能正好在高水位处，绕过等于不收缩；放弃作业的代价是重做几片，shrink 本来就是低频操作。',
  verify: 'g13 `stream` 段 + `verify`；g14 `reserve/extend` 段；`make asan`。',
  cost: 'shrink 时手头作业重做。',
  lesson: '新增一种"页的状态"时，要把所有枚举所有者的地方（verify / shrink / rebuild_free / compact_file）全部过一遍。',
  rel: [87, 101, 'r14-4'],
  code: { kind: 'real', src: '`backup/r13/fcs.h.pre-r13` → `backup/r13/fcs.h.final`，`shrink()` / `verify()`',
    old: `  RC shrink() {
      WGuard g(this); if (g.busy) return RC::BUSY;
      if (!wait_io()) return RC::TIMEOUT;
!     promote_pending(); u32 top = live_top();     // 不知道作业预留区
      ... truncate(top) ...                        // 预留区被截掉，作业下一片写到设备之外
  }
  // verify(): 页所有者枚举 {清单 run, free_, pending_free_, 清单页, SB} —— 没有"作业预留"⇒ page unowned`,
    new: `  RC shrink() {
      WGuard g(this); if (g.busy) return RC::BUSY;
      if (!wait_io()) return RC::TIMEOUT;
!     abandon_job();                               // 坑 108：分步作业的流式预留可能在活区之上，先归还
      promote_pending(); u32 top = live_top();
      ...
  }
+ // verify(): owner 8 = 作业预留（job_ && job_->w.reserved 区间）⇒ 不再报 page unowned`
  },
  old: [
    { t: '作业 J 开始：源 R（页 10~40），流式预留产物区 **页 100~130**（owner 8）。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'free:41-99 空闲:free', 'res:预留 100-130:res'] }], kv: { 'J 进度': '片 1/4', 'tail_': 131 } },
    { t: '提交点经过（作业跨提交点）。`verify`：100~130 既不在清单也不在 free_ ⇒ **page unowned**。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'free:41-99:free', 'res:100-130 无主？:bad'] }], kv: { 'verify': 'page unowned ✗' }, ok: false },
    { t: '应用调 `shrink()`：`live_top()` = 41 ⇒ `truncate(41)`。预留区被截掉。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'cut:41 … 已截:out'] }], kv: { 'tail_': 41, '预留区': '设备之外 ✗' } },
    { t: '作业下一片继续往 100~130 写 ⇒ 写到设备之外 ⇒ IO 错误 / 静默丢页。', rows: [{ n: '页', items: ['src:源 R:data', 'w:J 写页 101 → 设备之外:bad'] }], kv: { 'J 片 2': 'IO ✗' }, log: ['!dev write beyond capacity (page 101 >= 41)'], ok: false },
  ],
  new: [
    { t: '同左：J 预留 100~130。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'free:41-99:free', 'res:预留 100-130 (owner 8):res'] }], kv: { 'J 进度': '片 1/4' } },
    { t: '`verify` 认 owner 8：预留区有主。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'free:41-99:free', 'res:100-130 owner=8 ✓:res'] }], kv: { 'verify': 'OK ✓' }, ok: true },
    { t: '`shrink()` 开头 `abandon_job()`：预留归还 free_、作业作废（下次维护从头来）。然后才算 `live_top()` 并截断。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'cut:41 … 已截:out'] }], kv: { 'job_': 'null', 'tail_': 41 }, ok: true },
    { t: '下一次维护重开作业，在新高水位内预留。代价：重做几片；shrink 是低频操作。', rows: [{ n: '页', items: ['src:源 R 10-40:data', 'res2:新预留 41-71:res'] }], kv: { 'J\'': '片 1/4' }, ok: true },
  ],
});

PIT({
  id: 109, round: 13, cat: '并发 / 观测接口', found: 'TSan（第 13 轮收尾，旁边跑 g6 抢 CPU 第 1 次就抓到）', tags: ['mode 3', 'RGuard', 'run_count', 'lvl_hist', 'data race', 'TSan'],
  sym: 'TSan 报 data race：主线程 `run_count()` 读 `runs_.size()`，维护线程同时在 `merge_job_step` 里 `runs_.erase`。间歇：裸跑 6 次不出，旁边跑 g6 抢 CPU 第 1 次就抓到。',
  cause: '`run_count / lvl_hist / cell_count / seg_max / max_segs_per_cell` 一直不拿锁——mode 3 之前只有调用线程改账，这是对的；mode 3 引入后台维护线程后，这些"只读账"接口没有被重新审视。',
  rule: '**加后台线程时，要把所有"只读账"的接口全部过一遍**，不能只看数据路径。读未加锁的 `std::map::size()` 与并发 `erase` 是 UB，不只是"值旧一点"。',
  fix: '这 5 个接口拿 `RGuard`（本线程已持独占 ⇒ 空操作，所以 select 回调里也能叫）。',
  why: '不用 atomic 计数器镜像：`lvl_hist` / `seg_max` 需要遍历结构，镜像要在每个修改点维护，容易再漏；共享锁一次解决。',
  verify: 'asan 目标新增 `tsan_g13 mode3`、`tsan_g14 yield`；第 14 轮 TSan 循环 16 次 0 报告。',
  cost: '观测接口多一次共享锁获取（µs 级）。',
  lesson: '并发缺陷的复现需要**制造调度压力**（旁边跑别的负载），裸跑通过不算通过。',
  rel: [85, 86, 98],
  code: { kind: 'real', src: '`backup/r13/fcs.h.pre-r13` → `backup/r13/fcs.h.final`，观测接口',
    old: `- u32 run_count() const { return (u32)runs_.size(); }                    // 无锁读
- std::vector<u32> lvl_hist() const { ... 遍历 runs_ ... }                   // 无锁遍历
- u32 cell_count() const { return (u32)cells_.size(); }
  // mode 3 维护线程：merge_job_step() { ... runs_.erase(it); ... }           // 同时写`,
    new: `! u32 run_count() const { RGuard g(const_cast<Store*>(this)); return (u32)runs_.size(); }
  std::vector<u32> lvl_hist() const {
!     RGuard g(const_cast<Store*>(this));          // 本线程已持独占 ⇒ 空操作（select 回调里也能叫）
      ... 遍历 runs_ ...
  }
+ // cell_count / seg_max / max_segs_per_cell 同样加 RGuard
  // Makefile asan：$(BIN)/tsan_g13 mode3 ; $(BIN)/tsan_g14 yield`
  },
  old: [
    { t: 'mode 3：维护线程在作业里 `runs_.erase`；主线程同时调 `run_count()` 读 `runs_.size()`——**不拿锁**。', lanes: [{ n: '维护线程', ev: [{ a: 10, b: 60, l: 'merge_job_step: runs_.erase', k: 'lock' }] }, { n: '主线程', ev: [{ a: 30, b: 40, l: 'run_count() 无锁', k: 'wait' }] }], now: 35, kv: { '锁': '主线程未持有 ✗' } },
    { t: '裸跑 6 次没事（时序刚好错开）；旁边跑 g6 抢 CPU，第 1 次就撞上：TSan data race。', lanes: [{ n: '维护线程', ev: [{ a: 10, b: 60, l: 'runs_.erase', k: 'lock' }] }, { n: '主线程', ev: [{ a: 30, b: 40, l: 'runs_.size()', k: 'wait' }] }, { n: 'g6（抢 CPU）', ev: [{ a: 0, b: 100, l: '…', k: 'idle' }] }], now: 35, kv: { 'TSan': 'data race ✗' }, log: ['!ThreadSanitizer: data race on std::map::size / erase'], ok: false },
  ],
  new: [
    { t: '`run_count()` 先拿 `RGuard`（共享）：维护线程持独占时主线程等。', lanes: [{ n: '维护线程', ev: [{ a: 10, b: 60, l: 'merge_job_step（独占）', k: 'lock' }] }, { n: '主线程', ev: [{ a: 30, b: 60, l: '等共享锁', k: 'idle' }, { a: 60, b: 68, l: 'run_count()', k: 'ok' }] }], now: 64, kv: { '锁': '共享 ✓' }, ok: true },
    { t: '本线程已持独占时 `RGuard` 是空操作 ⇒ select 回调里也能叫。asan 目标加 `tsan_g13 mode3 / tsan_g14 yield`，第 14 轮循环 16 次 0 报告。', kv: { 'TSan 16 次': '0 报告 ✓' }, log: ['+tsan_loop14.done: runs=16 bad=0'], ok: true },
  ],
});

// ================= 第 14 轮：假象与设计性发现（ROUND14 §3，未编号） =================

PIT({
  id: 'r14-1', round: 14, cat: '测量方法 / 假象', found: '第 14 轮项 3 第二版 A/B', title: '读者 p99 "变差"是假象：写者没定节拍', tags: ['stall', 'pace_ms', 'p99', '节拍'],
  sides: ['不定节拍（第一次 A/B）', '定节拍 pace_ms=50（正确 A/B）'],
  sym: '项 3（读者让路）第二版 A/B：读者 p99 / >1 ms 次数看似**翻倍变差**，但机制上读者只会更少等。',
  cause: '不定节拍时写者不再等长单元，4 s 内提交次数 82~92 → 184~200；读者撞到更多**提交期 flush**（两次 fsync 在锁内），p99 自然上去。比较的不是同一个负载。',
  rule: '**比读者停顿必须定写者节拍**：被测变量（让路）改变了对照变量（提交速率）时，A/B 要先把对照变量钉住。',
  fix: '`stall` 第 8 参 `pace_ms`：写者每次提交至少间隔 50 ms（提交快了就 sleep 补足）。定节拍后 A/B 全面改善：mode 3 p99 59.6 → 17.6 µs、>1 ms 412 → 222。',
  verify: '`build/stall_old|stall 100000 100 4 0 0 {1,3} -1 50`（ROUND14 §4.3）。',
  cost: '定节拍下写者吞吐被人为压低，看的是停顿分布而不是吞吐。',
  lesson: '性能 A/B 里，任何"顺便变快"的东西都可能是干扰变量。',
  rel: [19, 'r14-2'],
  code: { kind: 'real', src: '`bench/probe/stall.cpp`（第 14 轮新增第 8 参）',
    old: `  // 写者线程：尽快提交
  while (running) {
      put_batch(); s.commit();          // 改后单元更短 ⇒ 提交翻倍 ⇒ 读者撞到更多 flush
  }`,
    new: `  // 写者线程：定节拍
+ const int pace_ms = argc > 8 ? atoi(argv[8]) : 0;
  while (running) {
+     auto t0 = now();
      put_batch(); s.commit();
!     if (pace_ms) sleep_until(t0 + pace_ms);   // 每次提交至少间隔 pace_ms ⇒ 两边提交次数一样
  }
  // maint: 行打印 units / yields / unit_max / units>4ms`
  },
  old: [
    { t: '老头（左）vs 新头（右）各跑 4 s：新头的维护单元短了，写者提交从 82 次变成 184 次。', lanes: [{ n: '老头写者', ev: [{ a: 0, b: 12, l: 'commit', k: 'io' }, { a: 12, b: 48, l: '长单元', k: 'lock' }, { a: 48, b: 60, l: 'commit', k: 'io' }, { a: 60, b: 96, l: '长单元', k: 'lock' }] }, { n: '新头写者', ev: [{ a: 0, b: 12, l: 'c', k: 'io' }, { a: 12, b: 24, l: 'u', k: 'lock' }, { a: 24, b: 36, l: 'c', k: 'io' }, { a: 36, b: 48, l: 'u', k: 'lock' }, { a: 48, b: 60, l: 'c', k: 'io' }, { a: 60, b: 72, l: 'u', k: 'lock' }, { a: 72, b: 84, l: 'c', k: 'io' }, { a: 84, b: 96, l: 'u', k: 'lock' }] }], now: 96, kv: { '提交次数 老/新': '82 / 184' } },
    { t: '读者撞到的"提交期 flush（锁内两次 fsync）"翻倍 ⇒ p99 看似翻倍变差。结论错误：不是让路没用，是负载变了。', kv: { 'p99 老/新': '36 / 70 µs ✗（假象）', '>1 ms 老/新': '200 / 410 ✗' }, log: ['!A/B invalid: commits 82 vs 184 — different workload'], ok: false },
  ],
  new: [
    { t: '`pace_ms=50`：两边写者都每 50 ms 一次提交 ⇒ 提交次数相同，读者撞到的 flush 次数相同。', lanes: [{ n: '老头写者', ev: [{ a: 0, b: 10, l: 'c', k: 'io' }, { a: 10, b: 40, l: '单元', k: 'lock' }, { a: 50, b: 60, l: 'c', k: 'io' }, { a: 60, b: 90, l: '单元', k: 'lock' }] }, { n: '新头写者', ev: [{ a: 0, b: 10, l: 'c', k: 'io' }, { a: 10, b: 22, l: '让路', k: 'lock' }, { a: 50, b: 60, l: 'c', k: 'io' }, { a: 60, b: 72, l: '让路', k: 'lock' }] }], now: 90, kv: { '提交次数 老/新': '相同' } },
    { t: '现在才是同负载 A/B：mode 3 p99 59.6 → **17.6 µs**，>1 ms 412 → 222，读者吞吐 ×2.3。', kv: { 'p99 老/新': '59.6 / 17.6 µs ✓', '>1 ms 老/新': '412 / 222 ✓', '读者批次': '9319 / 21541' }, log: ['+build/stall 100000 100 4 0 0 3 -1 50'], ok: true },
  ],
});

PIT({
  id: 'r14-2', round: 14, cat: '提交路径 / 语义取舍', found: '第 14 轮项 3 分解读者停顿', title: '读者停顿地板 ≈ 5~6 ms：两次 fsync 在排他锁内', tags: ['fsync', 'XLock', 'commit', '可见先于持久'],
  sides: ['现状（本轮保留）', '假如把 fsync 移出排他锁（否决）'],
  sym: '读者让路做完后，单次 `get` 最大停顿仍 5~6 ms，再也压不下去。',
  cause: '提交路径 `flush_mem → wait_io → commit_delta → write_sb` 里两次 `dev_->sync()` 全在排他锁内；读者在这段时间只能等。',
  rule: '**可见性与持久性的顺序是语义**：当前承诺"读者看见的提交一定已经持久"。把 fsync 移出锁 = 读者可能看见还没落盘的提交 —— 掉电后它"看见过"的数据消失。',
  fix: '本轮不做。若要做，必须作为显式选项（`commit(durable_before_visible=false)`）并在文档里写清"可见先于持久"的后果。',
  why: '不是做不到，是不该悄悄做：这是一个 API 契约变更，需要用户决定。',
  verify: 'ROUND14 §3.2 / §5；stall `single max` 5.7 ms（mode 1）。',
  cost: '读者 p99.9 / max 被 fsync 地板钉住。',
  lesson: '优化到"地板"时先问地板是物理的还是语义的；语义地板要走 API 变更流程，不走性能优化流程。',
  rel: ['r14-1', 81, 84],
  code: { kind: 'sketch', src: '`include/fcs.h` `commit_locked()` 结构（示意）；右侧为**未采用**的改法',
    old: `  RC commit_locked(bool durable) {
      WGuard g(this);                        // 排他锁
      flush_mem();  wait_io();
      commit_delta(delta);  dev_->sync();    // fsync #1：数据页 / 清单页
!     write_sb();           dev_->sync();    // fsync #2：超级块  —— 读者在这 5~6 ms 里全等
      return RC::OK;                         // 返回后读者看见的提交一定已持久
  }`,
    new: `  RC commit_locked(bool durable) {
      { WGuard g(this); flush_mem(); wait_io(); commit_delta(delta); write_sb(); }   // 锁内只改内存 + 排队写
!     dev_->sync(); dev_->sync();            // 锁外 fsync：读者不等了……
!     // ……但另一个读者在两次 sync 之间已经 get 到了这次提交的值；此刻掉电 ⇒ 它"看见过"的数据消失
      return RC::OK;                         // 语义变成"可见先于持久" ⇒ 需要 API 层面的显式选项，本轮否决
  }`
  },
  old: [
    { t: '提交路径（排他锁内）：flush → fsync#1 → SB → fsync#2。读者 R 在锁外等。', lanes: [{ n: '写者', ev: [{ a: 0, b: 15, l: 'flush', k: 'lock' }, { a: 15, b: 45, l: 'fsync #1', k: 'io' }, { a: 45, b: 55, l: 'SB', k: 'lock' }, { a: 55, b: 85, l: 'fsync #2', k: 'io' }] }, { n: '读者 R', ev: [{ a: 10, b: 85, l: '等锁 5~6 ms', k: 'wait' }, { a: 85, b: 92, l: 'get', k: 'ok' }] }], now: 85, kv: { 'R 看见的提交': '已持久 ✓', 'R 停顿': '5.7 ms（地板）' }, ok: true },
    { t: '这是语义地板：返回 / 可见之前，数据已经在盘上。t=30 掉电也无妨——读者还没看见任何东西。本轮保留。', lanes: [{ n: '写者', ev: [{ a: 0, b: 15, l: 'flush', k: 'lock' }, { a: 15, b: 30, l: 'fsync #1…', k: 'io' }, { a: 30, b: 32, l: '⚡', k: 'wait' }] }, { n: '读者 R', ev: [{ a: 10, b: 30, l: '等锁', k: 'wait' }] }], now: 30, kv: { '承诺': '可见 ⇒ 已持久 ✓', '重开后': 'R 没看见过 v，无矛盾' }, ok: true },
  ],
  new: [
    { t: '假如 fsync 出锁：锁内只改内存 + 排队写；读者 R 立刻拿到锁，读到新值 v。', lanes: [{ n: '写者', ev: [{ a: 0, b: 20, l: '锁内', k: 'lock' }, { a: 20, b: 50, l: 'fsync #1（锁外）', k: 'io' }, { a: 50, b: 80, l: 'fsync #2', k: 'io' }] }, { n: '读者 R', ev: [{ a: 10, b: 20, l: '等', k: 'wait' }, { a: 20, b: 27, l: 'get → v', k: 'ok' }] }], now: 27, kv: { 'R 停顿': '<1 ms', 'v 已持久？': '否' } },
    { t: 't=30 掉电：v 从未落盘。R（或它通知的下游）已经"看见过" v ⇒ 可见先于持久，语义破。需要显式 API 选项才能做，本轮否决。', lanes: [{ n: '写者', ev: [{ a: 0, b: 20, l: '锁内', k: 'lock' }, { a: 20, b: 30, l: 'fsync #1…', k: 'io' }, { a: 30, b: 32, l: '⚡', k: 'wait' }] }, { n: '读者 R', ev: [{ a: 20, b: 27, l: 'get → v', k: 'ok' }] }], now: 30, kv: { '重开后 get': 'NOTFOUND ✗（R 看见过 v）' }, log: ['!visible-before-durable: observed value lost after crash'], ok: false },
  ],
});

PIT({
  id: 'r14-3', round: 14, cat: '增量账 / 让路', found: 'g14 `yield` 段的账零漂移检查', title: '切片让路 `cut` 之后必须退掉 `j.acc` 里 `lo >= cut` 的登记', tags: ['merge_job_step', 'j.acc', 'cut', 'jacc', 'yield'],
  sym: '项 1 的单元级增量账在让路点**重复累计**：让路后收尾用公式更新 live/dead，与 recount 有漂移。',
  cause: '一片归并途中让路（`cut`）时，`cut` 之后的单元格已经在 `j.acc` 里登记了部分计数（em / sup / vis_old），但这些单元格的归并实际没完成，下一片从 `cut` 重新开始会**再登记一次**。',
  rule: '**增量账的登记必须和"工作真正完成"同步**：被回滚 / 推迟的工作，其账也要回滚。',
  fix: '让路时 `while (!j.acc.empty() && j.acc.back().c->lo >= cut) { 清 jacc/jdirty; pop }`，然后 `j.next_lo = cut; return MORE`。',
  verify: 'g14 `yield`：quantum 1 ms 让路 37 次、账零漂移；FCS_TRACE 对账 0 mismatch。',
  cost: '无。',
  lesson: '任何"提前返回"的路径都要检查：这一轮已经写下的中间状态里，哪些是以"会做完"为前提的。',
  rel: [15, 34, 'r14-1'],
  code: { kind: 'real', src: '`include/fcs.h` `merge_job_step()` 让路分支（第 14 轮）',
    old: `  // 一片归并（第 14 轮项 3 让路第一版）
  RC rc = merge_slice(j, cut);          // 让路时 cut = 第一个没归并完的单元 lo
  if (cut != ~0ull) {
!     j.next_lo = cut;                  // 下一片从 cut 重来 —— 但 j.acc 里 lo >= cut 的登记还在
      return RC::MORE;
  }`,
    new: `  RC rc = merge_slice(j, cut);
  if (cut != ~0ull) {                   // 让了路：cut 之后的单元格没归并，退掉它们
!     while (!j.acc.empty() && j.acc.back().c->lo >= cut) { j.acc.back().c->jacc = 0; j.acc.back().c->jdirty = 0; j.acc.pop_back(); }
      ++maint_yields_;
      j.next_lo = cut;
      return RC::MORE;
  }`
  },
  old: [
    { t: '片 k 归并单元 A、B、C；C 刚登记了部分计数（em=12）时到了 1 ms 时间片 ⇒ 让路，`cut = C.lo`。', rows: [{ n: 'j.acc（本片登记）', items: ['A:A em=40:data', 'B:B em=35:data', 'C:C em=12（未完）:pend'] }], kv: { 'cut': 'C.lo', '返回': 'MORE' } },
    { t: '片 k+1 从 `cut` 重来：C 从头归并，**再登记** em=50。收尾时 C 的 em = 12 + 50。', rows: [{ n: 'j.acc', items: ['A:A em=40:data', 'B:B em=35:data', 'C:C em=12:pend', 'C2:C em=50:bad'] }], kv: { 'C.em 合计': '62 ✗（真值 50）' } },
    { t: '收尾公式用错账更新 live/dead ⇒ 与 recount 漂移。', kv: { 'recount vs 账': '漂移 ✗' }, log: ['!JOB MISMATCH cell C: live by formula != recount'], ok: false },
  ],
  new: [
    { t: '让路时先 pop `j.acc` 里 `lo >= cut` 的登记（C 的部分计数清掉，`jacc/jdirty` 归零）。', rows: [{ n: 'j.acc', items: ['A:A em=40:data', 'B:B em=35:data', 'C:C 退掉:del'] }], kv: { 'cut': 'C.lo', 'maint_yields_': '+1' } },
    { t: '片 k+1 从 C 重来，登记 em=50 —— 只有一份。', rows: [{ n: 'j.acc', items: ['A:A em=40:data', 'B:B em=35:data', 'C:C em=50:new'] }], kv: { 'C.em 合计': '50 ✓' } },
    { t: 'g14 `yield`：让路 37 次、账零漂移；FCS_TRACE 对账 0 mismatch。', kv: { 'recount vs 账': '0 漂移 ✓' }, log: ['+g14 yield: yields=37 drift=0'], ok: true },
  ],
});

PIT({
  id: 'r14-4', round: 14, cat: '文件高水位 / 流式预留', found: 'g14 `extend` 段与 layout 探针', title: '就地续租会被清单页堵死 ⇒ 预留区旁留守卫洞', tags: ['extend_reserve', '守卫洞', '清单页', '续租', '搬家'],
  sym: '流式预留按存活字节估、估小了就"就地续租"（`extend_reserve`）——但续租经常失败，退化成整段搬家（多写一遍已写的产物）。',
  cause: '作业跨提交点：提交的清单页 / 小 L0 run 没洞可用时就**贴着预留区上方**长尾，预留区右边被占，续租无处可续。',
  rule: '**可增长的区间旁边要有增长余地**：预留区上方留一个守卫洞（≥ 清单页簇 + 小 run 的典型大小），让跨提交点的小分配落到别处。',
  fix: '`begin_stream` 在预留区之上留守卫洞（不登记为 tailhole，`alloc_pages` 第一遍跳过）；续租先试 `grow`、再试 `need`；都失败才搬家（`stream=false`）。不保证：清单链轮转后仍可能堵住（文档明说）。',
  verify: 'g14 `extend`：extends=2 moves=0；layout 200k 峰值 2.95× → 2.60×。',
  cost: '守卫洞是暂时的碎片（作业结束回收）。',
  lesson: '估计量 + 续租是对的，但续租的成功率取决于邻居的行为；把邻居挡在外面比把估计做准更可靠。',
  rel: [108, 'r14-5', 71],
  code: { kind: 'real', src: '`include/fcs.h` `begin_stream()` / 续租分支（第 14 轮）',
    old: `  // 第 14 轮项 2 第一版：按存活字节估 cap，估小了就续租
  bool begin_stream(RunWriter& w, src, cap_hint) {
      u32 cap = est_live_pages(src) + slack;
!     w.p0 = alloc_pages(cap);                    // 紧挨着上一个分配 ⇒ 提交点经过后清单页贴着 p0+cap 长
      ...
  }
  // 续租：extend_reserve(w, grow) 失败 ⇒ w.stream=false ⇒ 整段搬家`,
    new: `  bool begin_stream(RunWriter& w, src, cap_hint) {
      u32 cap = est_live_pages(src) + slack;
      w.p0 = alloc_pages(cap);
!     if (w.p0 + cap == tail_) tail_ += GUARD;      // 预留区之上留守卫洞（不登记 tailhole，alloc_pages 第一遍跳过）
      ...
  }
  // 续租：
! if (dbg_stream_cap_ || !(extend_reserve(w, grow) || extend_reserve(w, need))) { w.stream = false; return; }   // 先 grow 再 need，都不行才搬家
  ++stream_extends_;`
  },
  old: [
    { t: '作业预留 100~130（估 30 页）。提交点经过：1 页清单页 + 小 L0 run 没洞可用 ⇒ **贴着 131 长尾**。', rows: [{ n: '页', items: ['res:预留 100-130:res', 'man:清单 131:man', 'l0:小 run 132-135:data'] }], kv: { 'tail_': 136 } },
    { t: '产物实际需要 38 页 ⇒ 续租 131~138：被清单页占住 ⇒ **失败**。', rows: [{ n: '页', items: ['res:预留 100-130:res', 'man:清单 131 挡路:bad', 'l0:小 run 132-135:data'] }], kv: { 'extend_reserve': 'false ✗' } },
    { t: '退化成搬家：新分配 38 页，把已写的 30 页再拷一遍。写放大 + 峰值都上去。', rows: [{ n: '页', items: ['res:旧预留 100-130（作废）:del', 'man:131:man', 'l0:132-135:data', 'mv:新 136-173（重拷）:bad'] }], kv: { 'moves': '1 ✗', '多写': '30 页' }, ok: false },
  ],
  new: [
    { t: '作业预留 100~130，上方留**守卫洞** 131~138（不登记为 tailhole）。', rows: [{ n: '页', items: ['res:预留 100-130:res', 'g:守卫洞 131-138:hole'] }], kv: { 'tail_': 139 } },
    { t: '提交点经过：清单页 / 小 run 的分配第一遍跳过守卫洞 ⇒ 落到 139 之后。', rows: [{ n: '页', items: ['res:预留 100-130:res', 'g:守卫洞 131-138:hole', 'man:清单 139:man', 'l0:小 run 140-143:data'] }] },
    { t: '续租 131~138 成功（`extends=1, moves=0`）。守卫洞在作业结束时回收。', rows: [{ n: '页', items: ['res:预留 100-138（续租）:res', 'man:清单 139:man', 'l0:140-143:data'] }], kv: { 'extend_reserve': 'true ✓', 'moves': 0 }, log: ['+g14 extend: extends=2 moves=0'], ok: true },
  ],
});

PIT({
  id: 'r14-5', round: 14, cat: '文件高水位 / 碎片', found: 'layout 探针末态洞直方图', title: '相邻 run 之间的 1 页清单页把洞切碎 ⇒ 清单页簇', tags: ['man_hole_', '清单页簇', 'alloc_man_page_raw', '碎片', 'largest_free'],
  sym: '一批 L0 run 退休后，本应是 1 个 2152 页的大洞，实际是 **8 个 269 页的洞**；下一代产物装不进任何一个 ⇒ 只能长尾。',
  cause: '每个提交点写 1 页清单页，而清单页按规则 4 在尾部 `tail_++` 分配 ⇒ 恰好夹在相邻 run 之间。run 退休后清单页还活着（链没轮转），洞被切成"块 − 1 页"的小段。',
  rule: '**不同生命周期的对象不要交错摆放**：清单页寿命（链轮转）和 run 寿命（合并）不同，交错就是碎片。',
  fix: '清单页长尾时一次长 8 页、7 页留作"清单页簇"（`man_hole_`）；`alloc_pages` 第一遍跳过 ≤8 页的簇洞、也不拿它和尾部拼块；`tail_` 回落时簇（≤8 页且起点在 `man_hole_`）不收，并进大洞的簇照收。尽力而为、不持久（`rebuild_free` 清空）。',
  verify: 'layout 800k 末态 1.74× → 1.53×；簇对 200k/800k 末态 hw 略差 1~2%（保留）。',
  cost: '簇洞是最多 7 页的暂时空隙；簇不持久（重开后重新形成）。',
  lesson: '碎片问题先画布局图（layout 探针）再改分配器；没有图之前的"最佳适配"改动都回退了。',
  rel: ['r14-4', 17, 18, 70],
  code: { kind: 'real', src: '`include/fcs.h` `alloc_man_page_raw()` 规则 4 与 `alloc_pages()` 第一遍（第 14 轮）',
    old: `  u32 alloc_man_page_raw() {
      ... 规则 0~3：从空闲表拿 1 页 ...
!     // 规则 4：长尾 1 页
      u32 p = tail_++;                     // 每个提交点 1 页，夹在相邻 run 之间
      return p;
  }`,
    new: `  u32 alloc_man_page_raw() {
+     // 规则 0：簇里有页先用簇（man_hole_）
+     if (!man_hole_.empty()) { auto it = free_.find(*man_hole_.begin()); if (it != free_.end()) { ... take1(it, p0); man_hole_.insert(p0 + 1); return p0; } }
      ... 规则 1~3 ...
!     // 规则 4：长尾时一次长 8 页，1 页给清单、7 页登记为簇洞（不登记 tailhole）
      u32 p = tail_; tail_ += 8; free_[p + 1] = 7; man_hole_.insert(p + 1);
      return p;
  }
  // alloc_pages 第一遍：if (n <= 8 && man_hole_.count(p0)) continue;   // 跳过簇洞，也不和尾部拼块`
  },
  old: [
    { t: '8 次提交：每次 1 条 269 页的 L0 run + 1 页清单页，交错排列。', rows: [{ n: '页', items: ['r1:run 269:data', 'm1:清:man', 'r2:run 269:data', 'm2:清:man', 'r3:run 269:data', 'm3:清:man', 'r4:…:data', 'm4:清:man'] }], kv: { 'run 数': 8 } },
    { t: '8 条 run 合并成 L1 产物后退休；清单页还活着（链没轮转）⇒ 洞被切成 8 个 269 页。', rows: [{ n: '页', items: ['r1:洞 269:hole', 'm1:清:man', 'r2:洞 269:hole', 'm2:清:man', 'r3:洞 269:hole', 'm3:清:man', 'r4:…:hole', 'm4:清:man'] }], kv: { 'largest_free': '269 ✗', '总空闲': 2152 } },
    { t: '下一代产物 1700 页：没有一个洞装得下 ⇒ 长尾。高水位上去。', rows: [{ n: '页', items: ['h:8 × 269 洞:hole', 'm:清单 ×8:man', 'p:产物 1700（长尾）:bad'] }], kv: { 'tail_': '+1700 ✗' }, ok: false },
  ],
  new: [
    { t: '清单页长尾时一次长 8 页：1 页用、7 页留作簇（`man_hole_`）。后续 7 个提交点的清单页都落在簇里。', rows: [{ n: '页', items: ['r1:run 269:data', 'c:清单簇 8 页:man', 'r2:run 269:data', 'r3:run 269:data', 'r4:run 269:data', 'r5:…:data'] }], kv: { 'man_hole_': '{簇起点}' } },
    { t: 'run 分配第一遍跳过 ≤8 页的簇洞、不与尾部拼块 ⇒ run 连续相邻。', rows: [{ n: '页', items: ['r1:run:data', 'c:簇:man', 'r2:run:data', 'r3:run:data', 'r4:run:data', 'r5:run:data'] }] },
    { t: '8 条 run 退休：一个 ≈2152 页的大洞（簇在一端）。下一代产物 1700 页就地装下，不长尾。', rows: [{ n: '页', items: ['r1:洞 269:hole', 'c:簇:man', 'big:大洞 ≈1883:hole'] }], kv: { 'largest_free': '1883 ✓' } },
    { t: '`tail_` 回落时：簇（≤8 页且在 `man_hole_`）不收；簇并进大洞后照收。layout 800k 末态 1.74× → 1.53×。', rows: [{ n: '页', items: ['r1:洞:hole', 'c:簇:man', 'p:产物 1700:good', 'h:洞:hole'] }], kv: { 'layout 800k 末态': '1.53× ✓' }, ok: true },
  ],
});

PIT({
  id: 'r14-6', round: 14, cat: '测量方法', found: 'bench4 / layout 量空间', title: '`tail_` 回落只在最后一批释放页过了提交点之后 ⇒ 量空间前再提交一次', tags: ['tail_ 回落', 'promote_pending', 'pending_free_', '测量'],
  sym: 'drain 维护之后立刻读 `tail_` / 文件高水位：数字比真实末态高几百页，而且跑一次一个样。',
  cause: '第 14 轮加的"顶部空闲段释放 ⇒ `tail_` 回落"发生在 `promote_pending` 末尾，而 `promote_pending` 只在提交点之后放行（坑 99 的闸）。最后一批退休 run 的页还在 `pending_free_` 里。',
  rule: '**量"末态"要先把状态机推到稳态**：drain → 一个空 `commit()`（放行 + 回落）→ 再量。',
  fix: 'layout / bench4 在 drain 后再做一次空 `commit()` 再量 `# fcs space`；文档明说。',
  verify: 'bench4 200k `hwpct=150`：17.6 MB（空提交后）；g14 `recede` 白盒段。',
  cost: '无。',
  lesson: '任何"事件之后才生效"的优化，都要同步改测量点。',
  rel: ['r14-7', 99],
  code: { kind: 'real', src: '`bench/bench4.cpp` 末尾（第 14 轮）/ `include/fcs.h` `promote_pending()` 末尾',
    old: `  // bench4 结尾：
  drain_maint(s);
! print_space(s);              // 此时最后一批释放页还在 pending_free_，tail_ 没回落`,
    new: `  // bench4 结尾：
  drain_maint(s);
! s.commit(); s.commit();      // 空提交：过提交点 ⇒ promote_pending 放行 ⇒ 顶部空闲 ⇒ tail_ 回落
  print_space(s);              // # fcs space: hw / live / largest_free / extents / hw_reclaims / pending
  // promote_pending() 末尾（引擎）：
+ auto last = std::prev(free_.end());
+ const bool cluster = last->second <= 8 && man_hole_.count(last->first);
+ if (last->first < tail_ && last->first + last->second >= tail_ && !cluster) { tail_ = last->first; free_.erase(last); }`
  },
  old: [
    { t: 'drain 之后：最后一批退休 run 的页（顶部 300 页）在 `pending_free_`，没过提交点。', rows: [{ n: '页', items: ['live:活数据:data', 'pf:顶部 300 页 pending:pend'] }], kv: { 'tail_': '537', '真实需要': '237' } },
    { t: '此刻量 `tail_` / 文件大小 ⇒ 537。跑一次一个样（取决于最后一批释放在哪）。', kv: { '量到的 hw': '537 ✗（虚高）' }, log: ['!# fcs space: hw=537 pending=300'], ok: false },
  ],
  new: [
    { t: 'drain 之后再做一次空 `commit()`：提交点 ⇒ `promote_pending` 放行 300 页 ⇒ 顶部空闲段 ⇒ `tail_` 回落。', rows: [{ n: '页', items: ['live:活数据:data', 'fr:顶部 300 页 → free_ → 回落:free'] }], kv: { 'tail_': '237 ✓' } },
    { t: '现在量：237。g14 `recede` 白盒：237 → 537 → 237。', kv: { '量到的 hw': '237 ✓', 'pending': 0 }, log: ['+# fcs space: hw=237 pending=0'], ok: true },
  ],
});

PIT({
  id: 'r14-7', round: 14, cat: '测量方法', found: 'layout 探针多次运行', title: 'mode 1 的维护进度按墙钟预算走 ⇒ 末态高水位不确定', tags: ['mode 1', '墙钟预算', '可复现性', 'layout'],
  sym: '同一负载跑两次，`layout` 的末态 hw 可差几百页；用它比较两个版本的高水位会得出相反结论。',
  cause: 'mode 1 每次提交后的维护按墙钟 20 ms 预算做单元：机器快慢 / 噪声决定每次做几个单元 ⇒ 合并发生的时刻不同 ⇒ 末态布局不同。',
  rule: '**带时间预算的系统，末态不是确定函数**；比较要用峰值（对时刻不敏感）或多次运行的分布。',
  fix: '文档明说；比高水位看 layout 的"峰值 tail"或多跑几次；断言留余量。',
  verify: 'ROUND14 §4.4：200k B=500 默认末态 2.48×（峰 2.60×），多次运行末态 ±2%。',
  cost: '无。',
  lesson: '先确认指标是否确定，再决定用"等于"还是"分布"来断言。',
  rel: ['r14-6', 'r14-1'],
  code: { kind: 'sketch', src: '`bench/probe/layout.cpp` 输出字段（示意）',
    old: `  // 比较两个版本：只看末态
! printf("end tail=%u\\n", s.tail_);        // 两次运行可差几百页 ⇒ A/B 结论随机`,
    new: `  // 比较两个版本：看峰值 + 多次
! printf("peak tail=%u end tail=%u\\n", peak_tail, s.tail_);   // 峰值对合并时刻不敏感
  // 断言：末态 <= 预期 × 1.05（留余量）；高水位结论用 3 次运行的中位`
  },
  old: [
    { t: '同一负载跑两次：第 1 次最后一个合并在第 398 次提交发生，末态 tail 5250。', kv: { 'run #1 末态': 5250 }, lanes: [{ n: 'run #1', ev: [{ a: 0, b: 80, l: '…', k: 'run' }, { a: 80, b: 90, l: '合并', k: 'lock' }] }], now: 100 },
    { t: '第 2 次机器稍忙，预算内少做几个单元，最后一个合并没赶上 ⇒ 末态 tail 5600。两次差 350 页。', kv: { 'run #2 末态': '5600 ✗（差 350）' }, lanes: [{ n: 'run #2', ev: [{ a: 0, b: 95, l: '…', k: 'run' }, { a: 95, b: 100, l: '合并未完成', k: 'wait' }] }], now: 100, ok: false },
  ],
  new: [
    { t: '看峰值：两次运行的峰值 tail 都 ≈ 5900（合并时刻的"活 + 产物"），对时刻不敏感。', kv: { 'run #1 峰值': 5900, 'run #2 峰值': 5910 }, ok: true },
    { t: '或多跑几次取中位；断言留余量。ROUND14 的高水位结论都这样给。', kv: { '末态 3 次中位': 5300, '断言': '≤ 预期 × 1.05' }, ok: true },
  ],
});
