// 第 3 批（b）：坑 53 ~ 68（第七批「链接管」→ 第八批「L4 范围墓碑 / L2 ghost / M1」→ 第 8 轮 g4/s2）。
// 无历史头：老代码按 §12 复原（sketch）；能从当前 include/fcs.h 原样引用的新代码标 half。数字来自 §12 当年实测。

PIT({
  id: 53, round: '第七批（链接管）', cat: '大值 / 链所有权', found: '每压实一次漏一份大值', tags: ['add_take', 'w.takes', 'own_chains', 'ovfl_ref'],
  sym: '每压实一次**漏一份大值**的空间。',
  cause: '`RunWriter::add_take()` 只把描述符写进页，链记在 `w.takes`；`finish_run` 把它记进 `ovfl_ref`（"别人的链"）就再也不释放。',
  rule: '**接管的链归接管者所有**：`own_chains` 同时收 `w.chains` 与 `w.takes`；`collect_ovfl()` 不再按 `ovfl_ref` 排除。',
  fix: '如右。',
  verify: 'A2 压实 N 次 used 不涨；`ownership_check`。',
  cost: '无。',
  lesson: '"引用"与"拥有"是两种关系，转移发生时要把条目从一个集合搬到另一个。',
  rel: [51, 52, 55],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @364 `own_chains`；老代码按 §12 第 53 条复原',
    old: `- nr->ovfl_ref = w.takes;             // 当"别人的链"记着 ⇒ 永不释放 ⇒ 漏`,
    new: `  std::vector<std::pair<u32, u32>> own_chains;    // 本 run 拥有的链（含接管来的）
! nr->own_chains = w.chains; nr->own_chains.insert(end, w.takes.begin(), w.takes.end());`
  },
  old: [
    { t: 'P 接管 S 的链 C；记进 `ovfl_ref`。', rows: [{ n: 'P', items: ['c:C（ovfl_ref）:data'] }] },
    { t: 'P 退休：只释放 own_chains ⇒ C 永远留着。', rows: [{ n: '页', items: ['c:C 泄漏:bad'] }], kv: { '每次压实': '漏 1 份 ✗' }, ok: false },
  ],
  new: [
    { t: 'C 进 `own_chains` ⇒ P 退休时释放。', rows: [{ n: '页', items: ['c:C:free'] }], ok: true },
  ],
});

PIT({
  id: 54, round: '第七批', cat: '性能 / 存活账遍历', found: 'bench5 consol 读页 2004', tags: ['merge_emit', 'need_vals=false', '大值读两遍'],
  sym: '压实把每个大值**读两遍**：`bench5 consol` 读页 2004。',
  cause: '`recount` / `verify` / `check_liveness` 走 `merge_emit` 时也读大值——它们只要字节数。',
  rule: '**只算账的遍历不读值**：`merge_emit(..., need_vals=false)`。',
  fix: '如右。',
  verify: '读页 2004 → 4；接管之后连一遍都不用读。',
  cost: '无。',
  lesson: '给通用遍历加一个"要不要值"的开关，比写两套遍历便宜得多。',
  rel: [66, 27],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3395~3398；老代码按 §12 第 54 条复原',
    old: `- merge_emit(c, cs, fn);                 // 总是物化大值`,
    new: `  // need_vals=false：调用方只要"这条记录占多少字节"（存活账），不要值。
  void merge_emit(Cell* c, std::vector<Cur>& cs, F&& fn, bool need_vals = true) { ... }
! recount: merge_emit(c, cs, fn, /*need_vals=*/false);`
  },
  old: [
    { t: 'recount 读 1000 条大值链。', kv: { '读页': '2004 ✗' }, ok: false },
  ],
  new: [
    { t: '只读描述符。', kv: { '读页': '4 ✓' }, ok: true },
  ],
});

PIT({
  id: 55, round: '第七批（链接管）', cat: '大值 / 链所有权', found: 'compact_file 后 ovfl data', tags: ['merge_emit', 'is_ovfl', 'took', '二选一'],
  sym: '同坑 52 的表现：`compact_file` 后 `ovfl data`。',
  cause: '`merge_emit` 按 `is_ovfl` 交描述符；"没接管"时值已被读成字节，回调仍 `add_take` ⇒ 新 run 把**不属于自己**的链记进 `ovfl_ref`，源 run 接着把链还了。',
  rule: '**物化过的值与接管来的描述符只能二选一**：单独的 `took` 布尔，只有真接管才交 `dsc`。',
  fix: '如右。',
  verify: 'compact_file + verify。',
  cost: '无。',
  lesson: '"这条记录是大值"和"这条记录的链已经转移"是两个独立事实。',
  rel: [51, 52, 53],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3427 `bool took`；老代码按 §12 第 55 条复原',
    old: `- if (is_ovfl) fn(k, ver, dsc, /*take=*/true);     // 没接管也交描述符`,
    new: `  bool took = false;                   // 真的接管了吗（没接管就别把描述符交出去）
  if (is_ovfl && can_take(src)) { took = true; ... }
! fn(k, ver, took ? dsc : bytes, took);`
  },
  old: [
    { t: '未接管（源 S 还活）：值已物化，但回调拿到 `take=true` ⇒ P 记下 S 的链。', rows: [{ n: '链 C', items: ['c:S 拥有 · P 也记着:bad'] }] },
    { t: 'S 退休释放 C ⇒ P 的描述符悬空。', kv: { 'verify': 'ovfl data ✗' }, ok: false },
  ],
  new: [
    { t: '`took=false` ⇒ P 写新链 / 内联值，不碰 C。', rows: [{ n: '链', items: ['c:C 归 S:data', 'n:P 自己的:good'] }], ok: true },
  ],
});

PIT({
  id: 56, round: '第七批', cat: '页缓存 / 指针生命周期', found: 'scrub 再踩坑 40', tags: ['read_ovfl', 'scrub', '先拷 16 B'],
  sym: '`scrub` 里"读描述符 → 读链 → 再用原页指针"又踩了一次坑 40。',
  cause: '同坑 29 / 40。',
  rule: '**读链前把 16 B 描述符拷出，读链后重新取页**——写进 `page()` 的契约注释，并让每个新读路径审计。',
  fix: '`scrub` 同 `verify` 修法。',
  verify: 'scrub 回归。',
  cost: '无。',
  lesson: '同一个坑踩三次（29/40/56）说明需要的是契约 + 审计清单，不是再修一处。',
  rel: [29, 40],
  code: { kind: 'sketch', src: '按 §12 第 56 条复原',
    old: `- read_ovfl(desc_of(vp), buf); ... 继续用 p`,
    new: `+ u8 d[16]; std::memcpy(d, vp, 16); read_ovfl(desc_of(d), buf); p = page(pid);`
  },
  old: [{ t: '原页指针在读链后指向别的页。', kv: { 'scrub': '误报 ✗' }, ok: false }],
  new: [{ t: '拷出 + 重取页。', kv: { 'scrub': '✓' }, ok: true }],
});

PIT({
  id: 57, round: '第七批（A2 大值）', cat: '空间 / 高水位', found: '数据静默消失', tags: ['live_top', 'shrink', '溢出链', '截到链中间'],
  sym: '`shrink()` 之后下一次写入压在链上，数据**静默消失**（不是 CRC 错，是读出别人的数据）。',
  cause: '`live_top()` 没把溢出链算进来，截断点落在链中间。',
  rule: '**高水位由一切活页决定**：run 块 + meta + 清单 + 溢出链（+ 坑 74 的未提交链）。',
  fix: '`live_top()` 遍历每条 run 的 `ovfl_loc` 取 `d.pg0 + d.npages` 的最大值。',
  verify: 'shrink + 大值回归。',
  cost: 'shrink 时读描述符。',
  lesson: '和坑 17 同一条：收缩前列出"所有占据高地址的对象"。',
  rel: [17, 45, 74],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2199 `live_top()`；老代码按 §12 第 57 条复原',
    old: `  u32 live_top() { u32 top = 2; for (runs_) top = max(top, page0 + run_block(r)); ...清单...; return top; }   // 链漏了`,
    new: `  u32 live_top() {
      u32 top = 2;
      for (auto& kv : runs_) { RunPtr r = kv.second; top = std::max(top, r->page0 + run_block(r));
          // 溢出链不在 run 的连续块里，但它和 run 一样是活数据：漏算的话 shrink 会把高水位截到链中间
!         if (r->novfl) { ensure_ovfl_loc(r); for (auto& e : r->ovfl_loc) { OvflDesc d = desc_of(pg_val_at(page(e.first), e.second)); top = std::max(top, d.pg0 + d.npages); } }
      }
      ...清单链...; for_each_mem_chain(...);   // 坑 74
      return top;
  }`
  },
  old: [
    { t: 'run 块到 100，链 C 在 150~153。`live_top()=100` ⇒ 截到 100。', rows: [{ n: '页', items: ['r:run 0-100:data', 'c:链 150-153:data'] }], kv: { 'tail_': '100 ✗' } },
    { t: '下一次写入落在 150 ⇒ 链被覆盖 ⇒ 读出别人的数据。', rows: [{ n: '页', items: ['r:run:data', 'c:链 ← 新 run 覆盖:bad'] }], ok: false },
  ],
  new: [
    { t: '`live_top()=154` ⇒ 链保住。', rows: [{ n: '页', items: ['r:run 0-100:data', 'c:链 150-153:good'] }], kv: { 'tail_': '154 ✓' }, ok: true },
  ],
});

PIT({
  id: 59, round: '第八批（L4）', cat: '范围墓碑 / 读路径复杂度', found: 'g2 5000 墓碑', tags: ['ranges_', 'maxhi', '前缀最大 hi', '按需重算'],
  sym: '几千个墓碑 × 每次点查 = O(墓碑数)；插入时全量重算前缀和又是 O(n²)（5000 个墓碑插入 54 ms）。',
  cause: '`ranges_` 按 `lo` 有序但 `hi` 无序，判"k 是否被盖"只能往回逐个走。',
  rule: '**每条目多存 8 B 的前缀最大 hi（`maxhi`）**，往回走时一旦 `maxhi <= k` 立刻停；前缀和**按需**（`maxhi_dirty_`）重算。',
  fix: '`ensure_maxhi()` 惰性重算；`range_hidden` 用 `maxhi` 剪枝。',
  verify: 'g2：5000 墓碑下 0.25 µs/次；插入 54 → 4 ms。',
  cost: '每条墓碑 8 B。',
  lesson: '有序容器上的区间查询，用一条单调的辅助量把"往回走"截断。',
  rel: [60, 62, 63],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @989~1000 `range_hidden`；老代码按 §12 第 59 条复原',
    old: `- for (auto it = ranges_.upper_bound(k); it != begin; ) { --it; if (it->hi > k && ...) return true; }   // 走到头：O(墓碑数)
- // 插入时：for (全部条目) 重算前缀 max ⇒ O(n²)`,
    new: `  bool range_hidden(u64 k, u32 ver, u32 view) const {
      if (ranges_.empty()) return false;
      ensure_maxhi();                                // 按需：maxhi_dirty_ 时才重算
      auto it = ranges_.upper_bound({k, ~0u});       // 第一个 lo > k
      while (it != ranges_.begin()) {
          --it; const RangeTomb& r = it->second;
!         if (r.maxhi <= k) return false;            // 更早的条目全部不可能覆盖 ⇒ 立刻停
          if (r.hi > k && r.ver <= view && r.ver > ver) return true;
      }
      return false;
  }`
  },
  old: [
    { t: '5000 条墓碑；查 k 往回走 5000 步。', kv: { '点查': 'O(5000) ✗', '插入 5000': '54 ms' }, ok: false },
  ],
  new: [
    { t: '`maxhi <= k` 剪枝：通常几步即停。', kv: { '点查': '0.25 µs ✓', '插入 5000': '4 ms' }, ok: true },
  ],
});

PIT({
  id: 60, round: '第八批（L4）', cat: '范围墓碑 / 存活账', found: 'live mismatch', tags: ['del_range', 'recount', '先收集受影响 cell', 'O(n²)'],
  sym: '`del_range` 后 `cell->live` 与 `verify` 口径分家（`live mismatch`），而且这些单元永远不会被当成有垃圾。',
  cause: '逻辑删除一个字节都没重写，但一批记录变成不可见——不 `recount` 账就不动。逐墓碑 recount 又是 O(n²)（66 → 496 ms）。',
  rule: '**先收集受影响的 cell 再统一重算**（后来进一步攒到提交 / verify 时 `settle_ranges`，坑 76）。',
  fix: '`apply_ranges` 收集 cell 集合 → 一次 recount。',
  verify: '5000 墓碑 66 ms 量级；`verify` 0 mismatch。',
  cost: '无。',
  lesson: '账的重算要批处理：按"受影响集合"去重后一次做。',
  rel: [15, 59, 63, 76],
  code: { kind: 'sketch', src: '按 §12 第 60 条复原',
    old: `- for (tomb : new_tombs) for (Cell* c : cells_in(tomb)) recount(c);      // 5000 × 单元 ⇒ 496 ms`,
    new: `+ std::set<Cell*> touched; for (tomb : new_tombs) for (Cell* c : cells_in(tomb)) touched.insert(c);
+ for (Cell* c : touched) recount(c);                                       // 去重后一次 ⇒ 66 ms`
  },
  old: [
    { t: '5000 墓碑逐个 recount，同一单元被重算上百次。', kv: { '耗时': '496 ms ✗' }, ok: false },
  ],
  new: [
    { t: '收集受影响单元去重后重算。', kv: { '耗时': '66 ms ✓' }, ok: true },
  ],
});

PIT({
  id: 61, round: '第八批 → 第 7 轮 M1', cat: '空间 / compact_file 大值', found: 'cfcurve +31%', tags: ['take_limit_', '链全部接管', '物化高位链', '1.01'],
  sym: '大值库 `compact_file` 之后反而**变大**：96905 → 127795 页（+31%），活页 96901。',
  cause: '`relocate()` / `consolidate` 把链**全部接管** ⇒ 链永远不动、`shrink()` 截不到高位的链。（最初以为需要链引用计数，M1 证明不用。）',
  rule: '**第二遍只物化"末页 > `take_limit_`（= 活页 + 32）"的链**（新链经 `alloc_pages` 落到低位的洞里），其余照旧接管。',
  fix: '`take_limit_` 门限；`compact_file` 第二遍按门限决定物化 / 接管。',
  verify: '`cfcurve` ratio 1.31 → **1.01**；`b1` +31% → **+0.8%**。',
  cost: '高位链一次重写。',
  lesson: '先找根因再上重机制——"引用计数"在这里是错误的药。',
  rel: [28, 70, 71],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2427 `take_limit_`；老代码按 §12 第 61 条复原',
    old: `- // 第二遍：链一律接管 ⇒ 高位链永远不动 ⇒ shrink 截不到`,
    new: `! take_limit_ = need ? T : NIL;          // T = 活页 + 32：末页高于它的链才物化（落到低位洞）
  // merge_emit：if (is_ovfl && chain_end(dsc) <= take_limit_) 接管 else 物化`
  },
  old: [
    { t: '活数据搬到低位，链留在高位（全部接管）。', rows: [{ n: '页', items: ['r:run 低位:data', 'h:洞:hole', 'c:链 高位:bad'] }], kv: { 'ratio': '1.31 ✗' }, ok: false },
  ],
  new: [
    { t: '高位链物化到低位洞，低位链接管。', rows: [{ n: '页', items: ['r:run:data', 'c:链（物化到洞）:good', 'h:洞:hole'] }], kv: { 'ratio': '1.01 ✓', 'b1': '+0.8%' }, ok: true },
  ],
});

PIT({
  id: 62, round: '第八批（L4）', cat: '范围墓碑 / 键', found: 'del_range(10,90) 后 del_range(10,20) ⇒ 21..89 复活', tags: ['ranges_ 键', '(lo, ver)', 'add_tomb'],
  sym: '同 `lo` 的第二条墓碑把宽的那条**换掉** ⇒ 21..89 复活。',
  cause: '`ranges_` 按 `lo` 做键。',
  rule: '**键 = `(lo, ver)`**，插入走 `add_tomb`（支配关系判断：只在新墓碑 `ver >=` 且 `hi >=` 时才吃掉旧条目）。',
  fix: '如右。',
  verify: 'L4 用例。',
  cost: '无。',
  lesson: '键的唯一性要由业务含义推导，不能从"方便查找"倒推。',
  rel: [59, 49],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1011 `add_tomb`；老代码按 §12 第 62 条复原',
    old: `- ranges_[lo] = RangeTomb{lo, hi, ver};          // 同 lo 覆盖 ⇒ 宽墓碑被窄的换掉`,
    new: `  void add_tomb(u64 lo, u64 hi, u32 ver) {
      auto it = ranges_.lower_bound({lo, 0});
      while (it != ranges_.end() && it->first.first == lo) {
          if (it->second.ver >= ver && it->second.hi >= hi) return;      // 已被更强的盖住
          const bool dominated = it->second.ver <= ver && it->second.hi <= hi;
          ... if (dominated && unneeded) it = ranges_.erase(it); else ++it;
      }
!     ranges_[{lo, ver}] = RangeTomb{lo, hi, hi, ver};
      maxhi_dirty_ = true;
  }`
  },
  old: [
    { t: '`del_range(10,90)` 后 `del_range(10,20)` 覆盖同 lo。', rows: [{ n: 'ranges_', items: ['a:[10,20):tomb'] }], kv: { 'get(50)': '复活 ✗' }, ok: false },
  ],
  new: [
    { t: '键 (lo, ver)：两条共存（窄的 hi 更小不支配宽的）。', rows: [{ n: 'ranges_', items: ['a:[10,90)@v1:tomb', 'b:[10,20)@v2:tomb'] }], kv: { 'get(50)': 'NOTFOUND ✓' }, ok: true },
  ],
});

PIT({
  id: 63, round: '第八批（L4）', cat: '范围墓碑 / 死活口径', found: 'delloop runs 20→138', tags: ['run_fully_hidden', 'range_hidden', 'probe_prev', 'apply_ranges 提前'],
  sym: '被整条盖住的 run 永远 `live=nrec`、永不退休，而且每次提交都被重扫一遍（`delloop`：runs 20→138、del 提交 2→68 ms）；`flush_mem` 二次扣减；被盖 run 晚一次提交才退休。',
  cause: '三处判"死/活"的路径都只看"有没有更新版本"，不看范围墓碑：(a) `exact_live_recs`/`retire_dead_runs`；(b) `probe_prev`；(c) 墓碑到 `commit_delta` 才落账。',
  rule: '**任何判死活的路径都必须同时看范围墓碑**：`run_fully_hidden()`（整条 run 的键域被同一条 `ver > run.ver` 的墓碑盖住 ⇒ 一页不读）+ 逐条 `range_hidden`；`apply_ranges(delta)` 提前到 `rebalance` 之前。',
  fix: '三处各修。',
  verify: 'delloop 恢复 runs 20 量级、del 提交 2 ms。',
  cost: '每次判定多一次 `range_hidden`（有 maxhi 剪枝）。',
  lesson: 'L4 最大的教训：新增一种"不可见"的来源，要列出全部判死活的路径逐个接上。',
  rel: [59, 75, 79, 92],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2600 `run_fully_hidden`；老代码按 §12 第 63 条复原',
    old: `- u32 exact_live_recs(RunPtr r) { for (每条记录) if (!newer_has(k)) ++n; return n; }   // 被整条盖住 ⇒ n = nrec`,
    new: `  bool run_fully_hidden(const Run* r) const {
      if (ranges_.empty() || r->nrec == 0) return false;
      ensure_maxhi();
      auto it = ranges_.upper_bound({r->first, ~0u});
      while (it != ranges_.begin()) {
          --it; const RangeTomb& t = it->second;
          if (t.maxhi <= r->first) return false;
!         if (t.lo <= r->first && t.hi > r->last && t.ver > r->ver) return true;   // 整条盖住 ⇒ 一页不读
      }
      return false;
  }
  u32 exact_live_recs(RunPtr r, ...) { if (run_fully_hidden(r)) return 0; ... 逐条 range_hidden ... }`
  },
  old: [
    { t: '`del_range` 盖住 run R 全部键域。老判定：无更新版本 ⇒ live=nrec。', lvl: { L0: ['R:R live=nrec:bad'] }, kv: { 'runs': '20 → 138 ✗', 'del 提交': '68 ms' }, ok: false },
  ],
  new: [
    { t: '`run_fully_hidden(R)` ⇒ 0 ⇒ 退休。', lvl: { L0: ['R:R 退休:del'] }, kv: { 'runs': '≈20 ✓', 'del 提交': '2 ms' }, ok: true },
  ],
});

PIT({
  id: 64, round: '第八批（A2）', cat: '大值 / 读路径', found: '未提交大值 get 返回 16 B', tags: ['MemTable', 'desc_of', 'read_ovfl', 'get_at'],
  sym: '未提交的大值 `get` 返回了 **16 B 描述符**而不是值。',
  cause: '`MemTable` 里溢出条目存的是描述符（len=16），`get_at` 原样抄给了调用方。',
  rule: '**内存表路径也要物化大值**：`desc_of` + `read_ovfl`。',
  fix: '`get_at` 遇 `ovfl` 走 `get_ovfl_into(desc_of(mem_.val(*me)), out, cap, olen)`。',
  verify: 'A2 未提交读用例。',
  cost: '无。',
  lesson: '每条读路径（内存表 / run / 游标）都要分别过一遍大值用例。',
  rel: [42, 77],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1213；老代码按 §12 第 64 条复原',
    old: `- if (me) { copy(out, mem_.val(*me), me->len); return OK; }     // 大值时 len=16：描述符原样抄出`,
    new: `! if (me) return me->ovfl ? get_ovfl_into(desc_of(mem_.val(*me)), out, cap, olen) : copy_inline(...);`
  },
  old: [{ t: '返回 16 B 描述符。', kv: { 'get 长度': '16 ✗' }, ok: false }],
  new: [{ t: '物化链。', kv: { 'get 长度': '5000 ✓' }, ok: true }],
});

PIT({
  id: 65, round: '第八批（L2）', cat: '缓存 / 散列', found: 'bigget 16 命中率 0', tags: ['ghost 表', '乘法散列高位', 'khash(pg0) & 1023', '等差碰撞'],
  sym: 'L2 的 ghost 表命中率 **0**、pread 仍 15008。',
  cause: '乘法散列取高 8 位映射 256 槽；链首页号是等差数列（步长 377 = 一批大值的页数），两两撞到同一槽。',
  rule: '**结构化整数键别用乘法散列的高位做小表索引**：改 `khash(pg0) & 1023`。',
  fix: '如右。',
  verify: '`bigget 16` 命中率恢复；pread 下降。',
  cost: '无。',
  lesson: '散列函数要针对实际键分布验证，等差 / 对齐的键最容易暴露弱散列。',
  rel: [66],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @4519；老代码按 §12 第 65 条复原',
    old: `- u32& g = ghost_[(pg0 * 0x9E3779B1u) >> 24];     // 等差 pg0 ⇒ 高 8 位两两相同`,
    new: `  // 用 khash 而不是乘法散列取高位：链首页号是等差数列（步长 = 一批大值的页数），
  // 乘法散列的高 8 位把它们两两撞到同一槽（实测 377/1131、756/1510 …），准入形同虚设
! u32& g = ovc_ghost_[khash(pg0) & 1023u];`
  },
  old: [{ t: '377 与 1131 同槽、756 与 1510 同槽……', kv: { '命中率': '0 ✗', 'pread': 15008 }, ok: false }],
  new: [{ t: 'khash 均匀。', kv: { '命中率': '✓' }, ok: true }],
});

PIT({
  id: 66, round: '第八批（L2）', cat: '缓存 / 污染', found: '冷读 +8~10%', tags: ['merge_emit', 'read_ovfl(cacheable)', 'ghost 误准入'],
  sym: '归并之后的冷读慢 8~10%。',
  cause: '`merge_emit` 读大值走 `read_ovfl(cacheable=true)`，把冷扫描的 ~40% 链灌进 ghost 表、误准入。',
  rule: '**一遍过的归并 / verify / scrub 不走读缓存**：`Cur::val(..., cacheable=false)`。',
  fix: '如右。',
  verify: '冷读恢复。',
  cost: '无。',
  lesson: '缓存准入要区分"用户读"与"维护扫描"。',
  rel: [54, 65],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @713~718；老代码按 §12 第 66 条复原',
    old: `- const u8* val(Store* s, u16 len) { ... s->read_ovfl(d, vb, /*cacheable=*/true); }`,
    new: `  // cacheable=false：归并/压实物化大值是一遍过的，别让它碰链缓存（连准入 ghost 表也不碰）
! const u8* val(Store* s, u16 len, bool cacheable = true) { ... }
  // merge_emit / verify / scrub 调用：cs[best].val(s, len, /*cacheable=*/false)`
  },
  old: [{ t: '归并灌入 40% 链 ⇒ 误准入。', kv: { '冷读': '+8~10% ✗' }, ok: false }],
  new: [{ t: '维护扫描不碰缓存。', kv: { '冷读': '基线 ✓' }, ok: true }],
});

PIT({
  id: 67, round: '第 8 轮（g4）', cat: '异步 / 预取', found: 'g4 第 1 段：select 80/3000、get 坏 1610', tags: ['prefetch', 'wait_page', 'hang_', '上一任旧页'],
  sym: '`select` 只回 80/3000 行，之后 `get` 坏 1610 个键，`verify` 报 `page kind`。',
  cause: '（第 5 轮 B2 埋的雷）`prefetch()` 直接 `read_multi` 填页缓存，绕过了 `wait_page`。`commit(false)` 刚把 run 交给后台线程、立刻 `select` ⇒ 预取读到该位置**上一任**的旧页，却以"有效缓存"身份留下；随后 `page()` 命中缓存返回旧页。',
  rule: '**在飞的页一律不预取**（截到第一张在飞页为止）；`scrub` / `raw_dump` 裸读前 `wait_io()`。',
  fix: '`prefetch` 查 `hang_`。',
  verify: 'g4 第 1 段慢写设备放大窗口：全绿。',
  cost: '异步在飞期间预取变短。',
  lesson: '任何绕过"等写落地"的快速路径都是定时炸弹。',
  rel: [25, 14, 68],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @3044 `prefetch`；老代码按 §12 第 67 条复原',
    old: `- void prefetch(u32 pid0, u32 n) { dev_->read_multi(pid0, n, cache_slots); mark_valid(...); }   // 不看在飞`,
    new: `  void prefetch(u32 pid0, u32 n) {
      ...
      // 坑 67：预取绕过了 wait_page。在飞的页一律不预取（截到第一张在飞页为止），让 page() 走它自己的等待路径。
!     if (async_ && inflight_.load(std::memory_order_acquire) > 0) {
          std::lock_guard<std::mutex> lk(io_mu_);
          for (u32 i = 0; i < n; ++i) if (hang_.count(pid0 + i)) { n = i; break; }
          if (!n) return;
      }`
  },
  old: [
    { t: '`commit(false)`：run N → 页 50~57 在飞。`select` 预取 50~57 ⇒ 读到上一任旧页，标为有效。', lanes: [{ n: 'worker', ev: [{ a: 10, b: 60, l: '写 50-57', k: 'io' }] }, { n: '前台', ev: [{ a: 15, b: 25, l: '预取 50-57（旧页）', k: 'wait' }] }], now: 20, rows: [{ n: '缓存', items: ['c:50-57 = 旧页（有效）:bad'] }] },
    { t: '`page()` 命中缓存 ⇒ 旧页 ⇒ select 80/3000。', kv: { 'select': '80/3000 ✗', 'get 坏键': 1610 }, ok: false },
  ],
  new: [
    { t: '预取截到第一张在飞页 ⇒ 不读 50~57；`page()` 走 `wait_page`。', lanes: [{ n: 'worker', ev: [{ a: 10, b: 60, l: '写 50-57', k: 'io' }] }, { n: '前台', ev: [{ a: 15, b: 60, l: 'wait_page', k: 'wait' }, { a: 60, b: 70, l: '读', k: 'ok' }] }], now: 60, kv: { 'select': '3000/3000 ✓' }, ok: true },
  ],
});

PIT({
  id: 68, round: '第 8 轮（s2）', cat: '清单 / 入账顺序', found: 's2 op 7310 丢 14 键、op 14599 库被清空', tags: ['pend_delta_', 'commit(false)', 'commit_delta', '僵尸 run'],
  sym: '重放（回退 / 重开）复活一条指向**已释放页**的僵尸 run：s2 里一次丢 14 个键，另一次回退 `CORRUPT`、库被清空。',
  cause: '`commit(false)` 刷出的 run 的 `M_RUN_ADD` 暂存在 `pend_delta_`，只有 durable `commit()` 才并入清单；而 `consolidate` / `relocate` / `checkpoint` / 层合并直接走 `commit_delta`，先把那条 run **合并掉（DEL 入账）**，下一次 `commit()` 才把过期的 ADD 追加进去。',
  rule: '**"已落页、未入账"的 run 必须先于引用它的 DEL 入账**：`commit_delta` 统一先并入 `pend_delta_`；`write_man_snapshot` 清 `pend_delta_`；`commit()` 早退也看 `pend_delta_`。',
  fix: '如右。',
  verify: 's2 长跑 + 回退逐键比对（坑 72）。',
  cost: '无。',
  lesson: '日志里 ADD/DEL 的顺序必须与因果一致；"延迟入账"的东西要在任何引用它的记录之前入账。',
  rel: [43, 44, 69, 80],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1807 / @1835 `pend_delta_`；老代码按 §12 第 68 条复原',
    old: `- // consolidate 等维护：
- commit_delta(delta);                 // DEL(N) 先入账；N 的 ADD 还躺在 pend_delta_ ⇒ 下次 commit 才追加 ⇒ 日志里 ADD 在 DEL 之后`,
    new: `  RC commit_delta(Delta& d) {
!     if (!pend_delta_.empty()) { 先把 pend_delta_ 并进本次清单页; pend_delta_.clear(); }   // ADD 先于 DEL
      ... 写 d ...
  }
  if (wr_gen_ == durable_gen_ && pend_delta_.empty() && gpend_ == 0) { ++commits_merged_; return RC::OK; }   // 早退也看它`
  },
  old: [
    { t: '`commit(false)` 刷出 N：ADD(N) 在 pend_delta_。维护合并掉 N：DEL(N) 直接入清单。', rows: [{ n: '清单', items: ['d:DEL N:tomb'] }, { n: 'pend_delta_', items: ['a:ADD N:pend'] }] },
    { t: '下次 commit 追加 ADD(N) ⇒ 日志顺序 DEL, ADD ⇒ 重放复活 N（页已释放 / 被别人占用）。', rows: [{ n: '清单', items: ['d:DEL N:tomb', 'a:ADD N（过期）:bad'] }], kv: { '重放': '僵尸 N ✗', 's2': '丢 14 键 / CORRUPT' }, ok: false },
  ],
  new: [
    { t: '`commit_delta` 先并入 pend_delta_ ⇒ 顺序 ADD, DEL。', rows: [{ n: '清单', items: ['a:ADD N:data', 'd:DEL N:tomb'] }], kv: { '重放': '✓' }, ok: true },
  ],
});
