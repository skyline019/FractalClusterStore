// 第 3 批（c）：坑 69 ~ 84（第 8 轮：s2 长跑、B3 分配、A2 大值、g5 随机交错、g6 掉电）。
// 无历史头：老代码按 §12 复原（sketch）；能从当前 include/fcs.h 原样引用的新代码标 half。数字来自 §12 当年实测。

PIT({
  id: 69, round: '第 8 轮（s2）', cat: '快照 / 语义', found: 's2 回退丢已可见写', tags: ['snapshot()', 'pend_delta_', 'mem_', '提交点'],
  sym: '`commit(false)` 之后立刻 `snapshot()`，回退会把这些**已对读者可见**的写丢掉。',
  cause: '`snapshot()` 只抓 `pend_`，不抓 `pend_delta_`、也不抓 `mem_` 里的逻辑提交。',
  rule: '**快照点 = 一个持久提交点**：`snapshot()` 先 `commit_locked(true)` 再抓（没东西可提交时是空操作）。',
  fix: '如右（第 12 轮改为 `commit_locked` 而非 `commit()`，避免 mode 1 的提交后维护混进来）。',
  verify: 's2 回退逐键比对。',
  cost: '快照时一次提交。',
  lesson: '把"快照"定义在系统里已有的强一致点上，比为它单独发明一套一致性便宜。',
  rel: [68, 72, 'r14-1'],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2502 `snapshot()`；老代码按 §12 第 69 条复原',
    old: `- RC snapshot() { Snap s; s.pend = pend_; s.cells = cells_; ... }     // 不含 pend_delta_ / mem_`,
    new: `  RC snapshot() {
      WGuard g(this); if (g.busy) return RC::BUSY;
      if (nsnap_ >= MAX_SNAP) return RC::FULL;
      // 快照点 = 一个**持久提交点**。先 commit() 再抓，语义才闭合。
!     if (RC rc = commit_locked(true); rc != RC::OK) return rc;
      tr(TR_SNAP, vers_, tail_, man_last_, (u16)nsnap_);
      ...`
  },
  old: [
    { t: '`commit(false)` 后 mem_/pend_delta_ 里有已可见的写；`snapshot()` 没包含它们。', rows: [{ n: '快照', items: ['s:pend_ 只有旧增量:man'] }, { n: '未抓', items: ['m:mem_ / pend_delta_:bad'] }] },
    { t: '回退 ⇒ 这些写消失。', kv: { 'get': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '先提交 ⇒ 快照即提交点。', rows: [{ n: '快照', items: ['s:提交点 v12:good'] }], kv: { '回退后 get': '✓' }, ok: true },
  ],
});

PIT({
  id: 70, round: '第 8 轮（s2 / B3）', cat: '空间 / 布局', found: 's2 used 386 vs live 184', tags: ['relocate', 'run_block', 'man_high_', 'alloc_man_page'],
  sym: '搬家白写一遍；或者洞 = 块 − 1 页之后每一遍都搬不下去（s2：used 386 vs live 184）。',
  cause: '(a) `relocate()` 判"有更低的洞"用 `npages` 而不是 `run_block`（数据 + meta + 附件）⇒ 洞装不下整块。(b) run 搬到低位后 `checkpoint` 的新清单页按"最低空闲页"分配，正好卡进低位大洞。',
  rule: '**判洞用整块尺寸**；**`compact_file` 期间清单页放到最高 run 块之上**（`man_high_` / `alloc_man_page`），收尾时若清单页还能更低就再写一次（1 页 + SB）。',
  fix: '如右。',
  verify: 'g4 第 4 段：6 轮搬家后 `used == live`、0 个空闲碎片。',
  cost: '收尾多写 1 页 + SB。',
  lesson: '元数据页的放置是布局算法的一部分；"最低地址优先"对数据对、对清单页在搬家期是错的。',
  rel: [17, 18, 61, 71, 73, 'r14-5'],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1991 / @2411 `ManHigh`；老代码按 §12 第 70 条复原',
    old: `- if (lowest_hole_fits(r->npages)) relocate(r);         // 洞装不下 run_block ⇒ 落回尾部
- // checkpoint：alloc_pages(1) 取最低空闲页 ⇒ 卡进低位大洞`,
    new: `! if (lowest_hole_fits(run_block(r))) relocate(r);       // 数据 + meta + 附件
  struct ManHigh { bool& f; explicit ManHigh(bool& x) : f(x) { f = true; } ~ManHigh() { f = false; } };
! ManHigh mh(man_high_);                                  // compact_file 期间：清单页放到最高 run 块之上
  // 收尾：若清单页还能更低 ⇒ 再写一次（1 页 + SB）`
  },
  old: [
    { t: '洞 9 页 @低位，run 块 10 页（npages 9 + meta 1）。按 npages 判"装得下" ⇒ 搬 ⇒ 落回尾部。', rows: [{ n: '页', items: ['h:洞 9:hole', 'd:…:data', 'r:run 块 10 → 尾部:bad'] }] },
    { t: '清单页卡进低位洞 ⇒ 洞 = 块 − 1 ⇒ 永远搬不下。', rows: [{ n: '页', items: ['m:清单页:man', 'h:洞 8:hole', 'r:块 10:data'] }], kv: { 'used/live': '386/184 ✗' }, ok: false },
  ],
  new: [
    { t: '按 `run_block` 判；清单页放最高块之上。', rows: [{ n: '页', items: ['r:块 → 低位:good', 'm:清单页（高处）:man'] }] },
    { t: '6 轮搬家后 used == live。', kv: { 'used/live': '== ✓', '碎片': 0 }, ok: true },
  ],
});

PIT({
  id: 71, round: '第 8 轮（b1）', cat: '空间 / 布局', found: 'b1 洞 30155 vs 块 30890', tags: ['弹跳', 'bounce_gain', 'take_limit_', 'force 重写'],
  sym: '洞比块小一点时直接搬搬不下去；无条件 force 重写在小库上纯浪费（s2 写页 +23%）。',
  cause: '洞 30155 页就在 30890 页的 run 块正下方：强制重写一次会先落到尾部、再由 `relocate` 落进合并后的大洞（2 次重写换回整个洞）。',
  rule: '**弹跳有判据**：`bounce_gain() >= 32 && >= 块/8` 或 `chain_top() > take_limit_` 才 force。',
  fix: '`bounce_gain(blk)`。',
  verify: 'b1 洞收回；s2 无回退长跑写页 +7%（代价）。',
  cost: 'compact_file 密集的 s2 写页 +7%。',
  lesson: '启发式要有量化门槛和代价记录。',
  rel: [61, 70],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2425 / @2444 `bounce_gain`；老代码按 §12 第 71 条复原',
    old: `- force_rewrite(r);                           // 无条件 ⇒ 小库写页 +23%`,
    new: `! const u32 gain = bounce_gain(blk);
  if ((gain >= 32 && gain >= blk / 8) || chain_top() > take_limit_) force_rewrite(r);
  u32 bounce_gain(u32& blk) const { ... 洞 + 块合并后能回收的页数 ... }`
  },
  old: [
    { t: '洞 30155 在块 30890 正下方：不弹跳 ⇒ 洞留着；无条件弹跳 ⇒ 小库浪费。', rows: [{ n: '页', items: ['h:洞 30155:hole', 'r:块 30890:data'] }], kv: { 's2 写页': '+23% ✗' }, ok: false },
  ],
  new: [
    { t: 'gain 大 ⇒ 弹跳两次换回整个洞；小库 gain 小 ⇒ 不弹。', rows: [{ n: '页', items: ['r:块 → 最低:good'] }], kv: { 's2 写页': '+7%' }, ok: true },
  ],
});

PIT({
  id: 72, round: '第 8 轮（s2）', cat: '测试方法论', found: '坑 67/68/69 两轮没被发现', tags: ['rollback', 'resync 真值', '逐键比对'],
  sym: 's2 以前每次 `rollback` 后用 `get` 重建真值再往下跑——坑 67/68/69 全在它眼皮底下跑了两轮。',
  cause: '"resync 真值"等于没测：引擎错了，真值跟着错。',
  rule: '**`snapshot()` 时存一份真值，回退后逐键比对**；`make test` 每段"回退 / 重开后对拍"的都应如此。',
  fix: 's2 / g 系列测具改造。',
  verify: '改造后立刻抓到 67/68/69。',
  cost: '测具内存多一份真值。',
  lesson: '测试的真值必须独立于被测系统。',
  rel: [67, 68, 69, 80],
  sides: ['旧测法', '新测法'],
  code: { kind: 'sketch', src: '按 §12 第 72 条复原（测试侧）',
    old: `- db.rollback(); truth.clear(); for (k : keys) if (db.get(k, v)) truth[k] = v;     // 真值从被测系统重建`,
    new: `+ on snapshot(): saved = truth;
+ db.rollback(); for (k : saved) assert(db.get(k) == saved[k]);                    // 逐键比对`
  },
  old: [{ t: '回退后以引擎输出为真值 ⇒ 丢键不可见。', kv: { '抓到的 bug': '0 ✗' }, ok: false }],
  new: [{ t: '独立真值逐键比对。', kv: { '抓到的 bug': '67 / 68 / 69 ✓' }, ok: true }],
});

PIT({
  id: 73, round: '第 8 轮（A2 / B3）', cat: '大值 / 漏链', found: 'A2 第一次跑就抓到', tags: ['drop_mem_chain', 'snap_safe', 'B3 分配规则四版'],
  sym: '同一批里把大值覆盖 / `del` 掉：链既不属于任何 run、也不在空闲表——进程活着就一直漏，重开后 `rebuild_free` 才收回。',
  cause: '`mem_` 里 16 B 描述符被覆盖时没人放链。',
  rule: '**覆盖 / 删除内存表里的大值时 `drop_mem_chain`**（`free_pages(..., snap_safe=true)`：链写在上个提交点之后，没有快照能引用它）。',
  fix: '如右。B3 分配规则在这之后试了四版才定：最低空闲页（卡清单页，坑 70）→ 最高活块之上（平时抬水位）→ 纯大小规则（搬家期挡路）→ **定稿 = 平时大小规则 + 搬家期位置规则（`man_high_`）**。',
  verify: 'A2 覆盖负载 used 稳定；`ownership_check`。',
  cost: '无。',
  lesson: '所有权的每一次"失去引用"都要配释放动作，内存表也不例外。',
  rel: [42, 45, 70, 74],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @980 / @1592 `drop_mem_chain`；老代码按 §12 第 73 条复原',
    old: `- mem_.put(k, new_val);                 // 旧条目是描述符 ⇒ 链无人释放`,
    new: `  if (old && old->ovfl) drop_mem_chain(*old);     // 坑 73：大值的链要跟着放掉
  void drop_mem_chain(const Rec& e) { OvflDesc d = desc_of(mem_.val(e)); free_pages(d.pg0, d.npages, /*snap_safe=*/true); }`
  },
  old: [
    { t: '`put(k, 大值 A)` → 链 CA；同批 `put(k, B)` 覆盖描述符。', rows: [{ n: '页', items: ['ca:链 CA（无人引用）:bad'] }], kv: { '漏': '直到重开 ✗' }, ok: false },
  ],
  new: [
    { t: '覆盖时 `drop_mem_chain` ⇒ CA 释放。', rows: [{ n: '页', items: ['ca:链 CA:free'] }], ok: true },
  ],
});

PIT({
  id: 74, round: '第 8 轮（g5）', cat: '空间 / 高水位', found: 'g5 seed 1 第 303 步', tags: ['shrink', '未提交大值链', 'for_each_mem_chain', '三处同步'],
  sym: '`SHRINK tail 37 -> 34` 把未提交大值的链截掉、下一次分配盖住它；提交后 run 的描述符指向别人的页（`get` NOTFOUND）。',
  cause: '大值在 `put` 时就落成链，只有内存表的描述符引用它；`live_top()` / `rebuild_free()` 只看 run 和清单。',
  rule: '**新的页引用来源必须同步三处**：`live_top` / `rebuild_free` / `ownership_check`（所有者 kind 7）。',
  fix: '`for_each_mem_chain` 接进三处。',
  verify: 'g5 全种子。',
  cost: '无。',
  lesson: '旧规则只说"新页类型"要同步；这里把规则扩成"新页**引用来源**"。',
  rel: [57, 45, 73],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2227 / @2231；老代码按 §12 第 74 条复原',
    old: `  u32 live_top() { ... runs_ ... 清单 ... return top; }     // 未提交链不在任何 run 里 ⇒ 漏`,
    new: `  // 坑 74：**未提交**的大值在 put 时就落成了链，只有内存表的描述符引用它。
! for_each_mem_chain([&](const OvflDesc& d) { top = std::max(top, d.pg0 + d.npages); });
  // rebuild_free() / ownership_check()（kind 7）同样接入 for_each_mem_chain`
  },
  old: [
    { t: '`ALLOC [34,+3) user`（未提交链）→ `SHRINK 37 → 34` → `ALLOC [34,+2)` 盖住。', rows: [{ n: '页 34-36', items: ['c:未提交链 → 被新分配覆盖:bad'] }], kv: { '提交后 get': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '`live_top()` 含内存链 ⇒ tail 停在 37。', rows: [{ n: '页 34-36', items: ['c:未提交链:good'] }], kv: { 'tail_': '37 ✓' }, ok: true },
  ],
});

PIT({
  id: 75, round: '第 8 轮（g5）', cat: '范围墓碑 / 版本', found: 'del_range(1,10); put(5); commit(); get(5) → NOTFOUND', tags: ['墓碑版本', 'open_vers()', '严格 >', 'PendRange.ver'],
  sym: '`del_range` 之后同批写的键被一起盖掉。',
  cause: '墓碑版本取提交版本，和同一次 flush 出来的 run 版本相等，`>=` 规则把 del_range **之后**写的键也盖掉。',
  rule: '**墓碑版本定在发出时刻的 `open_vers()`**（`PendRange{lo,hi,ver}` 带进清单）；`range_hidden` 改成严格 `>`；`run_fully_hidden` / `settle_ranges` 快路径同步。',
  fix: '如右。',
  verify: 'g5；L4 用例。',
  cost: '无。',
  lesson: '同一提交里的先后顺序需要比"提交版本"更细的刻度。',
  rel: [63, 76, 79],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @944 `open_vers()`、@999 严格 `>`；老代码按 §12 第 75 条复原',
    old: `- tomb.ver = commit_ver;                                     // 与同批 run 同版本
- if (r.hi > k && r.ver >= ver) return true;                  // >= ⇒ 同批之后的 put 也被盖`,
    new: `  u32 open_vers() const { return (u32)(vers_ + 1); }
! pend_ranges_.push_back(PendRange{lo, hi, open_vers()});     // 版本定在发出时刻
! if (r.hi > k && r.ver <= view && r.ver > ver) return true;  // 严格大于：同版本的写在墓碑之后`
  },
  old: [
    { t: '`del_range(1,10)`@v5；`put(5)` 进同批 run@v5。`>=` ⇒ 5 被盖。', kv: { 'get(5)': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '严格 `>`：run@v5 不被 墓碑@v5 盖。', kv: { 'get(5)': '✓' }, ok: true },
  ],
});

PIT({
  id: 76, round: '第 8 轮（g5）', cat: '范围墓碑 / 可见性', found: 'del_range 提交前 get 可见', tags: ['add_tomb 立即', 'settle_ranges', 'PendRange.acct', 'abort → reload'],
  sym: '`del_range` 提交前对 `get` 不可见（以前只进 `pend_ranges_`）。',
  cause: '读路径只查 `ranges_`。',
  rule: '**`del_range` 立即 `add_tomb` 进 `ranges_`**；内存表里已有的同范围条目就地改成点墓碑；存活账**攒到**提交 / `verify` 时一起结（`settle_ranges`），否则回到 66 → 496 ms 老路。`abort()` 要撤墓碑只能 `reload()`。',
  fix: '如右。',
  verify: 'g5；`settle_ranges` 对账。',
  cost: '`abort()` 走 `reload()`。',
  lesson: '可见性要即时，账可以延迟——两者分开设计。',
  rel: [60, 75, 78],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2676 `settle_ranges()`；老代码按 §12 第 76 条复原',
    old: `- RC del_range(lo, hi) { pend_ranges_.push_back({lo, hi}); return OK; }     // get 看不到`,
    new: `  RC del_range(lo, hi) {
!     add_tomb(lo, hi, open_vers());                 // 立即可见
      mem_.tombstone_in_range(lo, hi);              // 内存表同范围条目就地点墓碑
      pend_ranges_.push_back(PendRange{lo, hi, ver, /*acct=*/false});
  }
  // commit / verify：settle_ranges();   // 未提交的范围墓碑已生效：先把存活账结清`
  },
  old: [{ t: '`del_range` 后 `get` 仍返回旧值。', kv: { 'get(50)': '有值 ✗' }, ok: false }],
  new: [{ t: '立即 `add_tomb`；账延迟结算。', kv: { 'get(50)': 'NOTFOUND ✓', '账': '提交时结' }, ok: true }],
});

PIT({
  id: 77, round: '第 8 轮（g5）', cat: '游标 / 内存表', found: 'select 看不见未提交写', tags: ['Cursor::next', 'mem_sorted', 'gen / epoch', 'inplace_merge'],
  sym: '`get` 看得见未提交的 put/del，`select`/`count`/`scan` 看不见（同步模式下连 `commit(false)` 之后都看不见）。',
  cause: '游标不归并内存表。',
  rule: '**游标多归并一路内存表**（同键内存表赢、墓碑不吐、未提交大值物化到游标缓冲、`view` 过滤）；键序视图 `mem_sorted()` 按 `MemTable::gen()` 缓存，`epoch` 不变且只追加时只排新尾巴再 `inplace_merge`。',
  fix: '如右。中间踩过：没看 `epoch`，clear 之后长度又长回去就把新条目当旧前缀（g5 八个种子全红）。',
  verify: '全量重排 6 ms/次 → 增量 0.06 ms。',
  cost: '内存表键序视图一份。',
  lesson: '增量缓存要同时跟踪"长度"和"代际"，只看长度会被 clear 骗。',
  rel: [64, 13],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @973 / @1282 `mem_sorted`；老代码按 §12 第 77 条复原',
    old: `- // Cursor::next 只归并 run 的段；内存表不在视野里`,
    new: `  // 现在把内存表（键序视图，见 Store::mem_sorted）当作最新的一路一起归并
! const auto& ms = mem_sorted();        // 按 gen 缓存；epoch 不变且只追加 ⇒ 只排新尾巴 + inplace_merge
  ... 同键内存表赢、墓碑不吐、未提交大值物化、view 过滤 ...`
  },
  old: [{ t: '`put(k)` 未提交；`select` 不含 k。', kv: { 'select': '缺 k ✗' }, ok: false }],
  new: [{ t: '内存表一路归并。', kv: { 'select': '含 k ✓', 'mem_sorted': '0.06 ms' }, ok: true }],
});

PIT({
  id: 78, round: '第 8 轮（g5）', cat: '维护 / 半持久化', found: 'g5 352 键缺失的前身', tags: ['compact_file', '未提交墓碑', 'maint_commit', '维护先提交'],
  sym: '重开后是"部分删除"——既不是上个提交点也不是现在。',
  cause: '坑 76 让墓碑立即生效后，`compact_file` 的归并把被盖住的记录丢了，墓碑本身却没进清单。',
  rule: '两条路：维护只看已提交墓碑（要把"已提交视图"穿过全部路径），或**维护先提交**。选后者：`maint_commit`（有未提交的写才真提交）。',
  fix: '如右；顺手关掉 L2 残留「consolidate 不先 flush mem」。',
  verify: 'g5；代价 s2 CPU +20%。',
  cost: '维护极密的 s2 CPU +20%。',
  lesson: '维护操作的输入必须是一个一致的持久状态。',
  rel: [76, 69, 24],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1918 `maint_commit`；老代码按 §12 第 78 条复原',
    old: `- RC compact_file() { normalize(); ... }       // 直接在未提交状态上归并`,
    new: `  RC maint_commit() {
!     if (!(wr_gen_ == durable_gen_ && pend_delta_.empty() && gpend_ == 0 && pend_ranges_.empty())) {
          if (RC r = commit_locked(true); r != RC::OK) return r;   // 维护操作自己就是提交点
      }
      recount_all_unknown(); return RC::OK;
  }
  RC compact_file() { if (RC r = maint_commit(); r != RC::OK) return r; ... }`
  },
  old: [
    { t: '未提交墓碑生效；归并丢记录；墓碑不在清单。', rows: [{ n: '盘上', items: ['r:记录已丢:del', 't:墓碑缺席:bad'] }], kv: { '重开': '部分删除 ✗' }, ok: false },
  ],
  new: [
    { t: '`maint_commit` 先提交 ⇒ 墓碑入清单 ⇒ 再归并。', rows: [{ n: '盘上', items: ['t:M_RANGE:tomb', 'r:记录已丢（一致）:good'] }], ok: true },
  ],
});

PIT({
  id: 79, round: '第 8 轮（g5）', cat: '范围墓碑 / 账口径', found: 'g5 seed 7 live mismatch 24046 vs 24050', tags: ['点墓碑', 'blen &&', '4 B/条', 'FCS_VERIFY_DBG'],
  sym: '账差 **4 B/条**：`live mismatch` 24046 vs 24050。',
  cause: '`merge_emit`/`verify` 从不把点墓碑算作被范围墓碑盖住（`blen &&`），其字节一直记在 live；`probe_prev`/`exact_live_recs` 按"被盖"算 ⇒ 覆盖这种键时少扣一次。',
  rule: '**一个口径**：统一成归并口径。',
  fix: '`probe_prev` / `exact_live_recs` 同步 `blen &&`。',
  verify: '`FCS_VERIFY_DBG=1` 逐条对出来；g5 seed 7 绿。',
  cost: '无。',
  lesson: '两套实现同一条规则 = 迟早分叉；要么共用函数，要么用对账测试盯住。',
  rel: [63, 75, 15],
  code: { kind: 'sketch', src: '按 §12 第 79 条复原',
    old: `- // probe_prev：if (range_hidden(k, ver, view)) treat_as_dead;              // 点墓碑也算被盖 ⇒ 少扣 4 B`,
    new: `! // 与 merge_emit 一致：只有有值的记录才可能"被范围墓碑盖住"
  if (blen && range_hidden(k, ver, view)) treat_as_dead;`
  },
  old: [{ t: '两套口径对点墓碑分叉。', kv: { 'live': '24046 vs 24050 ✗' }, ok: false }],
  new: [{ t: '统一归并口径。', kv: { 'live': '一致 ✓' }, ok: true }],
});

PIT({
  id: 80, round: '第 8 轮（g5）', cat: '清单 / 重放', found: 'g5 seed 36 reopen 352 键 NOTFOUND', tags: ['(seq, page0) 串代', 'M_RUN_DEL', 'segdels_ 作废', '搬回原地'],
  sym: 'reopen 后 352 个键 NOTFOUND。',
  cause: '合并产物继承源的最大 seq、搬迁保留 seq、B3 让低位页立刻复用 ⇒ run 6 → 合并进 58 → 搬回 6，新旧两代 `(seq, page0)` **完全相同**；清单里老一代的 `M_SEG_DEL` 在重放时把新一代的段摘掉。',
  rule: '**`apply_man` 处理 `M_RUN_DEL` 时作废该 page0 的所有待摘段**（run 死了，它的段无所谓了；新一代的 SEG_DEL 在日志里更晚，不受影响）。',
  fix: '`segdels_` 延迟摘段 + `M_RUN_DEL` 时 `remove_if`。',
  verify: 'g5 seed 36；方法论：随机交错 + 重开 + 逐键比对。',
  cost: '无。',
  lesson: '单元测试永远不会恰好把一条 run 搬回原地——这类 bug 只有随机交错 + 重开 + 逐键比对抓得到。',
  rel: [4, 68, 72, 103],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3827 / @3842 `segdels_`；老代码按 §12 第 80 条复原',
    old: `- case M_SEG_DEL: erase_seg(m.key, seg_key(m.seq, m.page0));   // 立即摘：新一代同 (seq,page0) 的段被老 DEL 摘掉`,
    new: `  case M_RUN_DEL:
!     segdels_.erase(std::remove_if(segdels_.begin(), segdels_.end(), [&](auto& sd){ return rid_of(sd.second) == m.page0; }), segdels_.end());   // run 死 ⇒ 它的待摘段作废
      ...
  case M_SEG_DEL:
      segdels_.push_back({m.key, seg_key(m.seq, m.page0)});   // 等 attach_run 之后再摘`
  },
  old: [
    { t: 'run(seq 9, page0 6) → 合并 → 搬回 page0 6（seq 仍 9）。日志：SEG_DEL(9,6)［老］… ADD 新一代 (9,6)。', rows: [{ n: '日志', items: ['d:SEG_DEL (9,6) 老:tomb', 'a:ADD (9,6) 新:data'] }] },
    { t: '重放按身份摘段 ⇒ 新一代的段被摘。', kv: { 'reopen': '352 键 NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '`M_RUN_DEL(6)` 作废老 SEG_DEL；新一代段保留。', kv: { 'reopen': '✓' }, ok: true },
  ],
});

PIT({
  id: 81, round: '第 8 轮（g6 掉电）', cat: '持久化屏障', found: 'g6 seed 1', tags: ['write_sb', 'sync', 'fdatasync ×2', '半空 run'],
  sym: 'SB 落盘、数据页没落 ⇒ 重开指向半空的 run。',
  cause: '`write_sb()` 前没有持久化屏障：数据页 `write` 了但没 `sync` 就写 SB。',
  rule: '**`write_sb()` = `wait_io + sync → 写 SB → sync`**，每个持久提交点 2 次 fdatasync。',
  fix: '如右。',
  verify: 'g6 全种子（掉电注入 + 逐键比对上一个持久提交点）。',
  cost: '每次持久提交 2 次 sync。',
  lesson: '"写了"≠"落了"；提交点两侧各一道屏障。',
  rel: [82, 83, 84, 104],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3586 `write_sb()`；老代码按 §12 第 81 条复原',
    old: `- RC write_sb() { return write_sb_raw(); }      // 数据页可能还在设备队列里`,
    new: `  RC write_sb() {
!     if (!wait_io()) return RC::TIMEOUT;
!     dev_->sync();
      RC rc = write_sb_raw();
      if (rc != RC::OK) return rc;
!     dev_->sync();
      if (pend_delta_.empty()) ++sb_gen_;   // 提交点已落盘
      return RC::OK;
  }`
  },
  old: [
    { t: '数据页在队列、SB 先落盘；掉电。', lanes: [{ n: '数据页', ev: [{ a: 10, b: 70, l: '未落盘', k: 'wait' }] }, { n: 'SB', ev: [{ a: 20, b: 30, l: 'SB 落盘', k: 'io' }] }], now: 40, kv: { '重开': 'SB 指向半空 run ✗' }, ok: false },
  ],
  new: [
    { t: 'sync → SB → sync。', lanes: [{ n: '数据页', ev: [{ a: 10, b: 30, l: '写 + sync', k: 'io' }] }, { n: 'SB', ev: [{ a: 30, b: 45, l: 'SB + sync', k: 'ok' }] }], now: 45, kv: { '重开': '✓' }, ok: true },
  ],
});

PIT({
  id: 82, round: '第 8 轮（g6 掉电）', cat: 'SB 双槽协议', found: 'g6 seed 2', tags: ['shrink', '同版本重写同槽', 'tail_ > 设备长度'],
  sym: '掉电在写到一半 ⇒ 两个槽一个坏一个旧，旧的 tail 还是截断前的 ⇒ 挂载后 `tail_ > 设备长度`。',
  cause: '`shrink()` 同版本重写同一个 SB 槽。',
  rule: '**每次 SB 写都换槽、都前进版本**：`shrink` 也 `++vers_`。',
  fix: '如右。',
  verify: 'g6 seed 2。',
  cost: '无。',
  lesson: '双槽协议的前提是"永远不覆盖当前有效槽"，任何写 SB 的路径都要遵守。',
  rel: [81, 83, 84],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2238 `shrink()`（`++vers_` + `write_sb`）；老代码按 §12 第 82 条复原',
    old: `- RC shrink() { ... tail_ = top; write_sb_same_slot(); }     // 同版本同槽 ⇒ 撕裂后两槽一坏一旧`,
    new: `  RC shrink() { ... tail_ = top; rebuild_free(); ...
!     ++vers_;                 // 换槽写（write_sb_raw 用 sb_slot_ ^ 1）
      return write_sb(); }`
  },
  old: [
    { t: '槽 A v10（旧 tail 100）；shrink 同版本重写 A 到一半掉电。', rows: [{ n: 'SB 槽', items: ['a:A 撕裂:bad', 'b:B v9 tail 100:sb'] }], kv: { '挂载': 'tail 100 > 设备 60 ✗' }, ok: false },
  ],
  new: [
    { t: 'v11 写到 B；掉电撕裂 B ⇒ A v10 仍完好且 tail 正确。', rows: [{ n: 'SB 槽', items: ['a:A v10:sb', 'b:B v11（撕裂也无妨）:good'] }], ok: true },
  ],
});

PIT({
  id: 83, round: '第 8 轮（g6 掉电）', cat: 'SB 双槽协议', found: 'g6 seed 3', tags: ['vers_ & 1', 'commit(false)', 'sb_slot_'],
  sym: '连续两次持久提交落到**同一个槽** ⇒ 掉电把唯一有效的 SB 砸了。',
  cause: '槽位 = `vers_ & 1`，而 `commit(false)` 也加 `vers_` 但不写 SB ⇒ 奇偶被打乱。',
  rule: '**槽位用独立的 `sb_slot_`，每次 SB 写才翻**。',
  fix: '如右。',
  verify: 'g6 seed 3（seed 2 第 1280 步：回到 258 而不是 259）。',
  cost: '无。',
  lesson: '协议变量（槽位）不要从别的语义变量（版本）派生。',
  rel: [81, 82, 84],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3611 / @7033 `sb_slot_`；老代码按 §12 第 83 条复原',
    old: `- u32 slot = vers_ & 1;                 // commit(false) 只前移 vers_ 不写 SB ⇒ 两次持久提交同槽`,
    new: `  // 坑 83（g6）：双槽协议要求的是"每次 SB 写交替"，与版本奇偶无关
! u32 slot = sb_slot_ ^ 1u;
  ... erase(slot); write(slot, buf) ...
! sb_slot_ = slot;
  u32 sb_slot_ = 1;             // 上一次 SB 落在哪个槽（0/1）；下一次写另一个`
  },
  old: [
    { t: 'v10→槽 0；`commit(false)` v11（不写）；v12→槽 0 又是同槽 ⇒ 撕裂 ⇒ 只剩更老的 v9。', rows: [{ n: 'SB 槽', items: ['a:槽0 v12 撕裂（覆盖了 v10）:bad', 'b:槽1 v9:sb'] }], kv: { '回到': '258 而不是 259 ✗' }, ok: false },
  ],
  new: [
    { t: '`sb_slot_` 交替 ⇒ v12 写槽 1，v10 完好。', rows: [{ n: 'SB 槽', items: ['a:槽0 v10:sb', 'b:槽1 v12:good'] }], ok: true },
  ],
});

PIT({
  id: 84, round: '第 8 轮（g6 掉电）', cat: '收缩 / 挂载一致性', found: 'g6 seed 4', tags: ['truncate 先于 SB', 'tail_ > dev_pages_', '挂载时截 / 扩回去'],
  sym: '掉电在"截断设备"与"写 SB"之间 ⇒ 老 SB 的 tail 超过设备长度 ⇒ `alloc_pages` 往设备外写（RamDev 报 IO；FileDev 悄悄把文件撑回去）。',
  cause: '`shrink` 先截断设备再写 SB。',
  rule: '**挂载时 `dev_pages_ >= tail_` 不成立就按 SB 把设备截 / 扩回去**。',
  fix: '如右。g6 本身也踩了一脚：设备"死后"的截断不能改介质镜像。',
  verify: 'g6 seed 4；排查顺序：MOUNT 行 `oa/ob vers` + 追踪环 → `[dev] write FAILED` → `G6_WATCH=<pid>`。',
  cost: '挂载时一次 truncate。',
  lesson: '两个持久动作之间总会掉电；挂载逻辑要能从任意中间态收敛。',
  rel: [22, 81, 82, 83],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2585 / @3646；老代码按 §12 第 84 条复原',
    old: `- // mount：tail_ = sb.tail;           // 不与设备长度核对 ⇒ alloc_pages 越界`,
    new: `  // rollback：if (tail_ > dev_pages_) tail_ = dev_pages_;
  // mount：
! if (tail_ > dev_pages_) { 按 SB 把设备截 / 扩回去（truncate(tail_) 或夹到设备长度）; }`
  },
  old: [
    { t: '截断到 60 → 掉电 → SB 仍 tail 100。', rows: [{ n: '设备', items: ['d:60 页:data'] }, { n: 'SB', items: ['s:tail 100:sb'] }], kv: { 'alloc': '越界写 ✗' }, ok: false },
  ],
  new: [
    { t: '挂载核对 ⇒ 设备扩回 100 或 tail 夹到 60。', rows: [{ n: '设备', items: ['d:与 tail 一致:good'] }], ok: true },
  ],
});
