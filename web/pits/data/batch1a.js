// 第 1 批（a）：坑 92 ~ 101（第 11 ~ 12 轮）。真实代码来自 backup/r11、backup/r12 的历史头与 include/fcs.h。
// 故事板 DSL：每步 {t 旁白, rows/pages 页条, lvl 层, kv 变量, lanes 时间线, log 控制台, ok 对错, note 提示}
// 页条元素 'id:标签:类型[:角标]'；类型见页面图例。

PIT({
  id: 92, round: 11, cat: '退休判定 / 存活账', found: 'storm 探针 r（第 11 次讨论）', tags: ['exact_live_recs', '点墓碑', '退休', 'verify run live recs'],
  sym: '一条**全死**的 run（188 页）永远不退休：每次提交都被 `exact_live_recs` "校准"回 live=1；`verify` 报 `run live recs` 不一致；探针跑到结束这 188 页还钉在文件里。',
  cause: '单元压实把某个键**整个丢掉**之后（最新版本被范围墓碑盖住 ⇒ 归并不 emit，旧副本 / 点墓碑作为"同键旧副本"跳过），老 run 里那些记录物理上还在页里，但已经**不在任何段的键域内**——对任何视图都不可见。老 `exact_live_recs` 整条 run 逐页扫，把这种"悬空点墓碑"（没有更新版本、`len==0`）算成 live=1。',
  rule: '**可见性只由段定义**：一条记录"活着"的充要条件是"某个单元的某个段把它圈在键域里，而且归并口径认它是赢家"。run 本体是物理容器，不是可见性来源。退休判定必须和读路径 / verify 用同一口径。',
  fix: '`exact_live_recs(r)` 先从 `cells_[*].segs.find(seg_key(r))` 收集**仍被引用的键域 span**；没有任何单元引用 ⇒ 按定义返回 0（全死）；有 ⇒ 只扫 span 覆盖的页、只数 span 键域内的记录。顺带把扫描量从 `npages` 降到"被引用的页"。',
  why: '另一个方向是"压实时顺手把老 run 里的悬空记录物理删掉"——做不到：老 run 是只读产物，改它等于重写；而且其它单元可能还引用它的别的键域。只改判定口径既对又便宜。',
  verify: 'storm 探针 r：188 页的死 run 退休；`verify` 不再报 `run live recs`；g9 / 全量 `make test` 绿。',
  cost: '`exact_live_recs` 多了一次遍历 `cells_` 找段（O(单元)），但它只在退休前最后一道核对调用，频度低；扫描页数反而少了。',
  lesson: '任何"复核 / 校准"路径都必须和主口径（读路径、归并）使用同一个可见性定义，否则校准本身就是制造不一致的源头。',
  rel: [63, 94, 95],
  code: { kind: 'real', src: '`backup/r11/fcs.h.pre-r11` → `backup/r11/fcs.h.post-R1`，`exact_live_recs()`',
    old: `  u32 exact_live_recs(RunPtr r) {
      if (run_fully_hidden(r)) return 0;
      u32 n = 0;
-     for (u32 i = 0; i < r->npages; ++i) {          // 整条 run 逐页扫
-         const u8* src = page(r->page0 + i);
-         ...
-         for (每条记录 k, len) {
!             // 点墓碑：没有更新版本 ⇒ 当成 live=1
!             if (!newer_has(k, ...)) ++n;          // 悬空墓碑也算进来了
-         }
-     }
      return n;                                       // 188 页全死的 run 返回 1 ⇒ 永不退休
  }`,
    new: `  u32 exact_live_recs(RunPtr r) {
      if (run_fully_hidden(r)) return 0;
+     // 坑 92：只数**仍被某个单元的段引用**的键域。
+     struct Span { u64 klo, khi; u32 p0, p1; };
+     std::vector<Span> spans;
+     const u64 sk = seg_key(r->seq, r->page0);
+     for (auto& kv : cells_) {
+         auto f = kv.second->segs.find(sk);
+         if (f == kv.second->segs.end()) continue;
+         const Seg& sg = f->second;
+         if (sg.p0 >= sg.p1 || sg.klo >= sg.khi) continue;
+         spans.push_back({sg.klo, sg.khi, sg.p0, std::min(sg.p1, r->npages)});
+     }
!     if (spans.empty()) return 0;        // 没有任何单元引用 ⇒ 按定义全死
      u32 n = 0;
+     for (auto& sp : spans) for (u32 i = sp.p0; i < sp.p1; ++i) { ... 只数 [klo,khi) 内的赢家 ... }
      return n;
  }`
  },
  old: [
    { t: '初态：run **R**（3 页）承载键 10（点墓碑 †）、20、30；单元 C 的段 `seg(R)` 把 [0,100) 圈进来。', rows: [{ n: 'run R 的页', items: ['p0:10 †:tomb', 'p1:20:data', 'p2:30:data'] }, { n: '单元 C 的段', items: ['sR:seg(R) [0,100):data'] }], kv: { 'exact_live_recs(R)': 3, 'R 状态': '活' } },
    { t: '`del_range(0,100)` 提交：范围墓碑 **G** 盖住整个单元。', rows: [{ n: 'run R 的页', items: ['p0:10 †:tomb', 'p1:20:data', 'p2:30:data'] }, { n: '单元 C 的段', items: ['sR:seg(R) [0,100):data', 'G:G=[0,100)@v9:tomb'] }], kv: { 'exact_live_recs(R)': 3, 'R 状态': '活' } },
    { t: '单元压实：归并看到 10/20/30 全被 G 盖住 ⇒ 一条都不 emit；C 的 `seg(R)` **被摘掉**。R 的页还在盘上，但没有任何段指向它。', rows: [{ n: 'run R 的页（物理还在）', items: ['p0:10 †:dim', 'p1:20:dim', 'p2:30:dim'] }, { n: '单元 C 的段', items: ['G:G=[0,100)@v9:tomb'] }], kv: { '引用 R 的段': 0 } },
    { t: '退休前复核：老 `exact_live_recs(R)` **整条 run 逐页扫**，扫到 p0 的点墓碑 10†——"没有更新版本" ⇒ 算 live=1。', rows: [{ n: 'run R 的页', items: ['p0:10 †:bad', 'p1:20:dim', 'p2:30:dim'] }, { n: '单元 C 的段', items: ['G:G=[0,100)@v9:tomb'] }], kv: { 'exact_live_recs(R)': '1 ✗', 'R 状态': '校准回"活"' }, ok: false },
    { t: '每次提交都这样校准一遍 ⇒ 188 页的全死 run **永不退休**；`verify` 的口径是逐单元归并、只看段内 ⇒ 报 `run live recs` 不一致。', rows: [{ n: 'run R 的页（钉住到探针结束）', items: ['p0:10 †:bad', 'p1:20:bad', 'p2:30:bad'] }], kv: { '钉住页数': '188 ✗', 'verify': 'run live recs ✗' }, log: ['!verify: run live recs: counted 1, cells see 0', '!retire_dead_runs: R live=1 → skip'], ok: false },
  ],
  new: [
    { t: '初态同左：R 承载 10†、20、30，C 的 `seg(R)` 圈住 [0,100)。', rows: [{ n: 'run R 的页', items: ['p0:10 †:tomb', 'p1:20:data', 'p2:30:data'] }, { n: '单元 C 的段', items: ['sR:seg(R) [0,100):data'] }], kv: { 'exact_live_recs(R)': 3, 'R 状态': '活' } },
    { t: '`del_range(0,100)` 提交：G 盖住整个单元。', rows: [{ n: 'run R 的页', items: ['p0:10 †:tomb', 'p1:20:data', 'p2:30:data'] }, { n: '单元 C 的段', items: ['sR:seg(R) [0,100):data', 'G:G=[0,100)@v9:tomb'] }], kv: { 'exact_live_recs(R)': 3, 'R 状态': '活' } },
    { t: '单元压实同样摘掉 `seg(R)`。', rows: [{ n: 'run R 的页（物理还在）', items: ['p0:10 †:dim', 'p1:20:dim', 'p2:30:dim'] }, { n: '单元 C 的段', items: ['G:G=[0,100)@v9:tomb'] }], kv: { '引用 R 的段': 0 } },
    { t: '新 `exact_live_recs(R)` 先收集**仍引用 R 的段** ⇒ `spans` 为空 ⇒ 直接返回 0：按定义全死，一页都不用扫。', rows: [{ n: 'run R 的页（无人引用）', items: ['p0:10 †:dim', 'p1:20:dim', 'p2:30:dim'] }, { n: '单元 C 的段', items: ['G:G=[0,100)@v9:tomb'] }], kv: { 'spans': '∅', 'exact_live_recs(R)': '0 ✓' }, ok: true },
    { t: 'R 退休：3 页进 `pending_free_`，过提交点后回到 `free_`；verify 与归并口径一致。', rows: [{ n: 'run R 的页 → pending_free_', items: ['p0:p0:pend', 'p1:p1:pend', 'p2:p2:pend'] }], kv: { '钉住页数': '0 ✓', 'verify': 'OK ✓' }, log: ['+retire_dead_runs: R live=0 → DEL', '+promote_pending: 3 pages → free_'], ok: true },
  ],
});

PIT({
  id: 93, round: 11, cat: '范围墓碑 / 快照', found: 'storm 探针 rsb（第 11 次讨论）', tags: ['add_tomb', 'prune_tombs', '支配规则', 'snap_max'],
  sym: '持有快照 S 的读者，在新的 `del_range` 提交之后，从 S 的视图里读到了**早就被删掉**的键（复活）。',
  cause: '`add_tomb` 登记范围墓碑时有条"省清单条目"的规则：同 `lo` 的旧条目 A 被新条目 B 支配（`A.ver <= B.ver && A.hi <= B.hi`）就删掉 A。但"B 支配 A"只对视图 `V >= B.ver` 成立：快照 `A.ver <= S < B.ver` 做 `range_hidden(k, ver, S)` 时 B 太新不算数、A 又没了 ⇒ 被 A 删掉的键在 S 里复活。',
  rule: '**任何"旧状态可以丢"的推理都要对所有存活视图成立**，不只是对当前视图。快照都建在提交点上、B.ver 是当前开放版本 ⇒ "有快照还需要 A" ⇔ `snap_max() >= A.ver`。同 ver 的条目对所有视图等价，可以替换。',
  fix: '`add_tomb` 只在 `dominated && unneeded` 时删 A（`unneeded = A.ver == ver || nsnap_ == 0 || A.ver > smax`）；快照全部释放后由 `prune_tombs()` 按同一规则惰性剪枝（从新往旧走，`hi` 不超过"更新条目最大 hi"的旧条目对任何视图都多余）。查询侧本来就回溯同 `lo` 的全部条目，不用改。',
  why: '不能靠"快照期禁止 del_range"——快照是只读视图，不该阻塞写者；也不能把 A 复制进快照对象——`ranges_` 是全局索引，快照只记提交点。延迟剪枝最便宜。',
  verify: 'storm rsb 探针不再复活；g9 快照 + del_range 组合段；`make test` 绿。',
  cost: '快照期间同 `lo` 的墓碑会多留几条（查询回溯同 `lo` 全部条目，多几次比较）；释放快照时多一次 `prune_tombs()` 线性扫。',
  lesson: '"省空间"的合并规则必须带上**视图维度**：对当前视图多余 ≠ 对所有视图多余。',
  rel: [62, 63, 69, 95],
  code: { kind: 'real', src: '`backup/r11/fcs.h.pre-r11` → `backup/r11/fcs.h.post-R1`，`add_tomb()` / 新增 `prune_tombs()`',
    old: `  void add_tomb(u64 lo, u64 hi, u32 ver) {
      auto it = ranges_.lower_bound({lo, 0});
      while (it != ranges_.end() && it->first.first == lo) {
          if (it->second.ver >= ver && it->second.hi >= hi) return;   // 新条目被旧的支配 ⇒ 不插
!         if (it->second.ver <= ver && it->second.hi <= hi) it = ranges_.erase(it);  // 旧条目被支配 ⇒ 删
          else ++it;
      }
      ranges_[{lo, ver}] = RangeTomb{lo, hi, hi, ver};
      maxhi_dirty_ = true;
  }`,
    new: `  void add_tomb(u64 lo, u64 hi, u32 ver) {
      auto it = ranges_.lower_bound({lo, 0});
+     const Vers smax = nsnap_ ? snap_max() : 0;
      while (it != ranges_.end() && it->first.first == lo) {
          if (it->second.ver >= ver && it->second.hi >= hi) return;
+         const bool dominated = it->second.ver <= ver && it->second.hi <= hi;
!         const bool unneeded  = it->second.ver == ver || nsnap_ == 0 || it->second.ver > smax;
+         if (dominated && unneeded) it = ranges_.erase(it);
          else ++it;
      }
      ranges_[{lo, ver}] = RangeTomb{lo, hi, hi, ver};
      maxhi_dirty_ = true;
  }
+ // 快照释放后剪掉被支配的旧墓碑：同 lo 按 ver 升序，从新往旧走
+ void prune_tombs() {
+     if (nsnap_ || ranges_.size() < 2) return;
+     ... for 每个 lo 组：u64 maxhi = 最新条目.hi;
+         更旧条目.hi <= maxhi ⇒ erase; 否则 maxhi = 它的 hi ...
+ }
  // release_snapshot(): ... prune_tombs();   // 坑 93`
  },
  old: [
    { t: '`del_range(10,50)` 在 v5 提交 ⇒ 墓碑 **A**。键 20 对 v≥5 的视图都不可见。', rows: [{ n: 'ranges_（同 lo=10）', items: ['A:A=[10,50)@v5:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data'] }], kv: { 'get(20, view=5)': 'NOTFOUND ✓' } },
    { t: '读者在 v6 建快照 **S**（nsnap_=1，snap_max=6）。S 必须一直看到"20 已删"。', rows: [{ n: 'ranges_', items: ['A:A=[10,50)@v5:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data', 'S:快照 S @v6:lock'] }], kv: { 'snap_max()': 6, 'get(20, S)': 'NOTFOUND ✓' } },
    { t: '`del_range(10,90)` 在 v8 提交 ⇒ 墓碑 **B**。老 `add_tomb`：A 被 B 支配（ver 5≤8，hi 50≤90）⇒ **删掉 A**。', rows: [{ n: 'ranges_', items: ['A:A 被删:del', 'B:B=[10,90)@v8:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data', 'S:快照 S @v6:lock'] }], kv: { 'ranges_ 条数': 1 } },
    { t: 'S 读键 20：`range_hidden(20, 3, view=6)` 只认 `ver <= 6` 的墓碑 ⇒ B(v8) 不算数，A 已经没了 ⇒ **20 复活**。', rows: [{ n: 'ranges_', items: ['B:B=[10,90)@v8:dim'] }, { n: '数据', items: ['k20:key 20 @v3:bad', 'S:快照 S @v6:lock'] }], kv: { 'get(20, S)': '找到 @v3 ✗' }, log: ['!snapshot view 6: key 20 resurrected (deleted at v5)'], ok: false },
  ],
  new: [
    { t: '同左：A=[10,50)@v5。', rows: [{ n: 'ranges_（同 lo=10）', items: ['A:A=[10,50)@v5:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data'] }], kv: { 'get(20, view=5)': 'NOTFOUND ✓' } },
    { t: '快照 S@v6。', rows: [{ n: 'ranges_', items: ['A:A=[10,50)@v5:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data', 'S:快照 S @v6:lock'] }], kv: { 'snap_max()': 6 } },
    { t: 'B=[10,90)@v8 到来：A 被支配，但 `unneeded` = (5==8) ∥ (nsnap_==0) ∥ (5 > smax=6) = **false** ⇒ A **留下**，两条并存。', rows: [{ n: 'ranges_', items: ['A:A=[10,50)@v5:tomb', 'B:B=[10,90)@v8:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:data', 'S:快照 S @v6:lock'] }], kv: { 'dominated': 'true', 'unneeded': 'false', 'ranges_ 条数': 2 } },
    { t: 'S 读键 20：回溯同 lo 的全部条目，A(v5≤6) 命中 ⇒ NOTFOUND。当前视图 v8 由 B 盖住，也正确。', rows: [{ n: 'ranges_', items: ['A:A=[10,50)@v5:tomb', 'B:B=[10,90)@v8:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:dim', 'S:快照 S @v6:lock'] }], kv: { 'get(20, S)': 'NOTFOUND ✓', 'get(20, v8)': 'NOTFOUND ✓' }, ok: true },
    { t: 'S 释放 ⇒ `prune_tombs()`：同 lo 从新往旧走，A.hi=50 ≤ maxhi=90 ⇒ 现在才剪掉 A。省条目的目的照样达到，只是晚一点。', rows: [{ n: 'ranges_', items: ['A:A 剪掉:del', 'B:B=[10,90)@v8:tomb'] }, { n: '数据', items: ['k20:key 20 @v3:dim'] }], kv: { 'nsnap_': 0, 'ranges_ 条数': 1 }, log: ['+release_snapshot → prune_tombs: erased A (hi 50 <= 90)'], ok: true },
  ],
});

PIT({
  id: 94, round: 11, cat: '墓碑生命周期', found: '第 11 次讨论（老代码同样有）；g9 t94 系列', tags: ['点墓碑', 'older_may_contain', 'bloom', 'consolidate', 'dropped_tombs'],
  sym: '点墓碑**从不被清除**：连全库 `consolidate()` 之后墓碑还在产物里；删除多的负载墓碑只增不减，每次合并都原样搬运（白写）。',
  cause: '归并只有"同键更新版本盖住旧版本"这一条丢弃规则。墓碑是某个键的"最新版本"，没有东西盖它 ⇒ 永远是赢家 ⇒ 永远被搬进产物。而它存在的唯一意义——盖住**更老 run 里**的旧值——在下面已经没有更老副本时就消失了。',
  rule: '墓碑可以丢 ⇔ **物理上不存在更老的副本**（任何 seq 更小的 run 都不可能含这个键）。"更老"按全库 run 的 seq 判（`seg_key < 组内最老`），存在性用每条 run 的 bloom 判（可能有 ⇒ 保守留下）。',
  fix: '归并回调里：`l == 0 && !dsc && !older_may_contain(k, minkey)` ⇒ 丢墓碑（`++dropped_tombs`）。`older_may_contain` 遍历 `runs_` 中 `seg_key < minkey` 且键域相交的 run，查 bloom。账：墓碑以前对读可见（算在 `c->live`），丢了要进 `vis_old`；若它本来就被更新 run 盖住，进 `chk` 延后精确判。',
  why: '按"单元里有没有更老的段"判更老更便宜，但错了（坑 96）：段是可见性索引，不是存在性索引。bloom 误判率 ≈0.24%，误判只会**多留**墓碑，安全方向正确。',
  verify: 'g9 `t94a/b/c`：全库归一后 `tombstones()` 归零；覆盖 + 删除负载的合并写量下降；bench 的 dropped_tombs 计数可观测。',
  cost: '每条墓碑一次 bloom 探测 × 更老 run 数（只在归并时）；有更老 run 时墓碑仍往下带，直到它落到栈底。',
  lesson: '"赢家规则"只回答可见性；对象的**生命周期**（什么时候可以物理消失）要单独有一条带存在性判据的规则。',
  rel: [92, 96, 95, 107],
  code: { kind: 'real', src: '`backup/r11/fcs.h.post-R1` → `backup/r11/fcs.h.post-94`，`merge_run_set` 回调 / 新增 `older_may_contain()`',
    old: `  // merge_run_set(...) 的 emit 回调
  auto emit = [&](u64 k, const u8* p, u16 l, const u8* dsc, RunPtr sr) {
      // 同键旧副本已在 k 路归并里跳过；走到这里 = 本组里这个键的赢家
-     // 墓碑（l == 0）没有任何丢弃规则：它是赢家，照搬进产物
!     bool added = dsc ? w.add_take(k, v, dsc) : w.add(k, v, p, l);   // 墓碑也 add
      ...
  };
  // consolidate() 之后：tombstones() 仍 = 删除次数`,
    new: `+ bool older_may_contain(u64 k, u64 me) const {      // 物理存在性：更老的 run 里可能有 k 吗
+     const u64 hk = khash(k);
+     for (auto& kv : runs_) {
+         RunPtr r = kv.second;
+         if (!r->npages || seg_key(r->seq, r->page0) >= me) continue;   // 只看比组内最老还老的
+         if (k < r->first || k > r->last) continue;
+         if (r->bloom.may(hk)) return true;
+     }
+     return false;
+ }
  auto emit = [&](u64 k, const u8* p, u16 l, const u8* dsc, RunPtr sr) {
!     if (l == 0 && !dsc && !older_may_contain(k, minkey)) {   // 坑 94：下面没有更老的副本 ⇒ 墓碑不再往下带
+         if (a && vis) {
+             a->vis_old += osz;                 // 以前对读可见（算在 c->live 里），现在物理消失……
+             if (newer_may_contain(k, maxseq, 0)) chk.push_back(Pend{k, osz, (u32)ci, true, true});
+         }
+         ++dropped_tombs;
+         return true;                           // 丢
+     }
      bool added = dsc ? w.add_take(k, v, dsc) : w.add(k, v, p, l);
      ...
  };`
  },
  old: [
    { t: '三层各一条 run：L2 的 R1 有键 7 的值；L1 的 R4 有 **7†**（删除）；L0 的 R9 是新写。', lvl: { L0: ['R9:R9 {3,5}:data'], L1: ['R4:R4 {7†}:tomb'], L2: ['R1:R1 {7,8}:data'] }, kv: { '墓碑数': 1 } },
    { t: '合并 L1+L2（组 {R4,R1}）：7† 盖住 7 ⇒ 旧值丢；但 7† 自己是赢家，**原样搬进产物** P1。', lvl: { L0: ['R9:R9 {3,5}:data'], L1: [], L2: ['P1:P1 {7†,8}:new'] }, kv: { '墓碑数': 1, '产物里的墓碑': 1 } },
    { t: '再合并 L0+L2：7† 下面已经什么都没有了，还是赢家 ⇒ 又搬一次。', lvl: { L0: [], L1: [], L2: ['P2:P2 {3,5,7†,8}:new'] }, kv: { '墓碑数': 1, '搬运次数': 2 } },
    { t: '全库 `consolidate()` 之后：墓碑还在。删除多的负载里墓碑只增不减，每次合并都白搬一遍。', lvl: { L0: [], L1: [], L2: ['P3:P3 {…1000 条 †…}:bad'] }, kv: { '墓碑数': '1000 ✗', '每轮白写': '1000 条 ✗' }, log: ['!consolidate(): tombstones()=1000 (never decreases)'], ok: false },
  ],
  new: [
    { t: '同左初态。', lvl: { L0: ['R9:R9 {3,5}:data'], L1: ['R4:R4 {7†}:tomb'], L2: ['R1:R1 {7,8}:data'] }, kv: { '墓碑数': 1 } },
    { t: '合并 {R4,R1}：`minkey` = 组内最老 = R1。回调遇到 7†：`older_may_contain(7, R1)` 看比 R1 **更老**的 run——没有 ⇒ **丢墓碑**。', lvl: { L0: ['R9:R9 {3,5}:data'], L1: [], L2: ['P1:P1 {8}:new'] }, kv: { 'older_may_contain(7)': 'false', 'dropped_tombs': 1, '墓碑数': '0 ✓' }, ok: true },
    { t: '反例保护：如果 L3 还有一条更老的 R0 且 bloom 说"可能含 7"，墓碑就继续往下带（宁可多留）。', lvl: { L1: ['R4:R4 {7†}:tomb'], L2: ['R1:R1 {7,8}:data'], L3: ['R0:R0 bloom(7)=可能:dim'] }, kv: { 'older_may_contain(7)': 'true（bloom）', '动作': '保留 †' } },
    { t: '账：墓碑以前算在 `c->live`（对读可见），丢了要记 `vis_old`；若它本来已被更新 run 盖住则进 `chk` 延后精确判。全库归一后 `tombstones()` 归零。', lvl: { L2: ['P3:P3 {…0 条 †…}:good'] }, kv: { '墓碑数': '0 ✓', 'vis_old 已记': '✓' }, log: ['+consolidate(): tombstones()=0, dropped_tombs=1000'], ok: true },
  ],
});

PIT({
  id: 95, round: 11, cat: '存活账口径', found: '第 11 次讨论；快照钉住 run 时 verify `live mismatch`', tags: ['settle_ranges', 'probe_prev', 'merge_emit', 'exact_live_recs', '口径统一'],
  sym: '快照钉住 run 时 `verify` 报 `live mismatch`：`settle_ranges` 的"整盖快路径"把点墓碑记 0 字节，而归并口径把同一条墓碑算活——两套口径在能退休时互相掩盖，退不了休时就对不上。',
  cause: '"被更新的范围墓碑盖住的**点墓碑**算不算死"四处各有各的答案：`merge_emit` / `exact_live_recs` 算活（坑 79 的旧结论），`settle_ranges` 整盖快路径算 0 字节，`probe_prev` 介于两者之间。',
  rule: '统一口径：**被更新的范围墓碑盖住的点墓碑也是死的**。安全性：墓碑 T(ver t) 被 G(ver g>t) 盖住时，T 能盖的旧值（ver<t<g）同样被 G 盖住 ⇒ 对任何视图 V≥g，T 多余；视图 t≤V<g 只有快照，快照由退休 / 压实门禁保护。',
  fix: '四处同改：`probe_prev` 去掉 `blen &&` 的例外（墓碑也走 `range_hidden`）；`merge_emit` 对被盖住的墓碑同样丢；`exact_live_recs` 与 `settle_ranges` 快路径按同一判据。',
  why: '另一个方向"快路径也把墓碑算活"同样一致，但会让被整段删掉的单元永远有"活"字节、永远压实不干净。选"算死"是因为它和"范围墓碑之后记录物理消失"的终局一致。',
  verify: 'g9 快照 + del_range + 点删组合；`verify` 不再报 `live mismatch`；`make test` 绿。',
  cost: '无额外开销；只是一次性把四处判据对齐。',
  lesson: '同一个谓词（"这条记录死了吗"）在代码里只能有**一个实现**；每多一处手写判断，就多一个在边界条件下分叉的机会。',
  rel: [79, 63, 92, 94],
  code: { kind: 'real', src: '`backup/r11/fcs.h.pre-r11` → `backup/r11/fcs.h.post-R1`，`probe_prev()`（其余三处同口径）',
    old: `  // probe_prev：flush 时探测同键旧版本，决定扣多少 live
  // 坑 79（g5）：**点墓碑**例外 —— 归并口径从不把点墓碑算作被范围墓碑盖住，
  // 它的字节一直记在 live 里；这里若按"被盖"返回 false，覆盖它时就少扣一次。
! if (blen && range_hidden(key, bver, view)) return false;   // blen==0（墓碑）不走 range_hidden ⇒ 算活
  len = (u16)blen;
  ...
  // settle_ranges 整盖快路径：c->live = 0（墓碑按 0 字节）  ← 与上面矛盾`,
    new: `  // 坑 95：口径统一成 **被更新的范围墓碑盖住的点墓碑也是死的**
  //（merge_emit / exact_live_recs / settle_ranges 整盖快路径 / 这里，四处一致）
  // 安全性：T(ver t) 被 G(ver g>t) 盖住时，T 能盖的旧值 ver<t<g 也被 G 盖住，
  // 对任何视图 V>=g T 都多余；视图 t<=V<g 只有快照，由退休/压实门禁保护。
! if (range_hidden(key, bver, view)) return false;           // 墓碑一视同仁
  len = (u16)blen;
  ...
  // merge_emit：坑 95：点墓碑同样丢（被更新的范围墓碑盖住的墓碑对当前及之后的视图都多余）`
  },
  old: [
    { t: 'run R 里有键 1 的值和键 2 的点墓碑 **2†**(v3)；之后 `del_range` G(v5) 整盖该单元。', rows: [{ n: 'run R', items: ['k1:1 @v2:data', 'k2:2 † @v3:tomb'] }, { n: 'ranges_', items: ['G:G 整盖 @v5:tomb'] }], kv: { '单元 live（快路径）': '0 B', '归并口径 live': '1 条（2†）' } },
    { t: '`settle_ranges` 整盖快路径：单元 live 直接清零（墓碑按 0 字节）。归并口径（坑 79 旧结论）：2† 没被盖住，仍算活。**两个答案**。', rows: [{ n: 'run R', items: ['k1:1 @v2:del', 'k2:2 † @v3:bad'] }, { n: 'ranges_', items: ['G:G 整盖 @v5:tomb'] }], kv: { '快路径': '0 B', '归并 / exact_live_recs': '1 条 ✗' } },
    { t: '平时 R 很快退休，矛盾被掩盖。快照 S(v4) 钉住 R ⇒ 退不了休 ⇒ `verify` 把两套账一比：`live mismatch`。', rows: [{ n: 'run R（被 S 钉住）', items: ['k1:1 @v2:del', 'k2:2 † @v3:bad', 'S:快照 S @v4:lock'] }], kv: { 'verify': 'live mismatch ✗' }, log: ['!verify: cell live 0 vs recount 1 (tombstone 2)'], ok: false },
  ],
  new: [
    { t: '同左：R = {1, 2†(v3)}，G(v5) 整盖。', rows: [{ n: 'run R', items: ['k1:1 @v2:data', 'k2:2 † @v3:tomb'] }, { n: 'ranges_', items: ['G:G 整盖 @v5:tomb'] }], kv: { '口径': '四处同一' } },
    { t: '四处同一判据：被 G 盖住的点墓碑 2† **也是死的**——它能盖的旧值（ver<3）同样被 G 盖住，对 V≥5 多余。', rows: [{ n: 'run R', items: ['k1:1 @v2:del', 'k2:2 † @v3:del'] }, { n: 'ranges_', items: ['G:G 整盖 @v5:tomb'] }], kv: { '快路径': '0 B', '归并 / exact_live_recs': '0 条 ✓' }, ok: true },
    { t: '快照 S(v4) 钉住 R 也无妨：视图 3≤V<5 只有快照，S 读 2 时由 2† 盖住（墓碑物理还在，门禁保证 R 不被压实掉）；verify 两套账一致。', rows: [{ n: 'run R（被 S 钉住）', items: ['k1:1 @v2:del', 'k2:2 † @v3:del', 'S:快照 S @v4:lock'] }], kv: { 'verify': 'OK ✓', 'get(2, S)': 'NOTFOUND ✓' }, ok: true },
  ],
});

PIT({
  id: 96, round: 11, cat: '墓碑生命周期', found: 'g9 t94c 第 30 次提交（坑 94 落地前）', tags: ['older_may_contain', '段 ≠ 存在', '老值复活'],
  sym: '坑 94 的第一版落地后，g9 `t94c` 跑到第 30 次提交：一个早已删除的键 `get` 又返回了老值。',
  cause: '第一版判"下面还有没有更老的副本"用的是**单元里有没有更老的段**。但单元压实可以把老 run 的段从单元里摘掉（该键被盖住 ⇒ 不 emit ⇒ 段消失），老 run 本体还在 `runs_`。墓碑按"没有更老的段"被丢掉；之后层合并把老 run 的悬空老值归并进产物、重新挂回单元——此时墓碑已经没了。',
  rule: '**"存在更老版本"必须按物理存在判**：段只是可见性索引，不是存在性索引。能让旧值重新可见的任何路径（层合并重新挂段）都要被判据覆盖。',
  fix: '`older_may_contain(k, minkey)` 改为遍历 `runs_`：所有 `seg_key < 组内最老` 且键域含 k 的 run，查 bloom——有一个说"可能" 就保留墓碑。',
  why: '也可以在层合并"重新挂回"时再查墓碑，但那时墓碑已丢，信息不可恢复。只能在丢弃的那一刻用最保守的判据。',
  verify: 'g9 `t94c` 30+ 次提交不再复活；全量 `make test`。',
  cost: 'bloom 探测对象从"单元内段"变成"全库更老 run"，次数略多（只在归并丢墓碑时）。',
  lesson: '二级索引（段）会被维护操作主动删除；凡是回答"存在吗"的问题，只能问一级结构（run 本体）。',
  rel: [94, 92],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` 的 `older_may_contain()`；老代码按 g9 t94c 失败时的第一版复原',
    old: `  // 第一版（已否决）：按"这个单元里有没有比组更老的段"判
- bool older_in_cell(Cell* c, u64 minkey) {
-     for (auto& s : c->segs)
!         if (s.first < minkey) return true;     // 段被单元压实摘掉后 ⇒ false ⇒ 丢墓碑
-     return false;
- }
  ... if (l == 0 && !older_in_cell(a->c, minkey)) { ++dropped_tombs; return true; }`,
    new: `+ bool older_may_contain(u64 k, u64 me) const {   // 按物理存在判：遍历 runs_，不看段
+     const u64 hk = khash(k);
+     for (auto& kv : runs_) {
+         RunPtr r = kv.second;
!         if (!r->npages || seg_key(r->seq, r->page0) >= me) continue;   // 只看更老的 run 本体
+         if (k < r->first || k > r->last) continue;
+         if (r->bloom.may(hk)) return true;                             // 可能有 ⇒ 保留墓碑
+     }
+     return false;
+ }
  ... if (l == 0 && !dsc && !older_may_contain(k, minkey)) { ++dropped_tombs; return true; }`
  },
  old: [
    { t: '单元 C 有两段：R2（键 7 老值）与 R5（**7†**）。', rows: [{ n: '单元 C 的段', items: ['sR2:seg(R2) {7}:data', 'sR5:seg(R5) {7†}:tomb'] }, { n: 'runs_（物理）', items: ['R2:R2 {7}:data', 'R5:R5 {7†}:tomb'] }], kv: { 'get(7)': 'NOTFOUND ✓' } },
    { t: '单元压实：7 被 7† 盖住 ⇒ 不 emit ⇒ C 里 **`seg(R2)` 被摘掉**。R2 本体还在 `runs_`（别的单元可能还引用它）。', rows: [{ n: '单元 C 的段', items: ['sR5:seg(R5) {7†}:tomb'] }, { n: 'runs_（物理）', items: ['R2:R2 {7} 本体还在:dim', 'R5:R5 {7†}:tomb'] }], kv: { 'C 里更老的段': 0 } },
    { t: 'R5 参加层合并：第一版判据 "C 里有没有更老的段？" ⇒ 没有 ⇒ **丢掉 7†**。', rows: [{ n: '单元 C 的段', items: ['P1:seg(P1) {}:new'] }, { n: 'runs_（物理）', items: ['R2:R2 {7} 本体还在:dim', 'P1:P1 {}:new'] }], kv: { 'older_in_cell(7)': 'false', '7†': '已丢 ✗' } },
    { t: '后续层合并把 R2 并进产物 P2，P2 的段重新挂回 C ⇒ 老值 7 **复活**，墓碑已经不存在。', rows: [{ n: '单元 C 的段', items: ['P2:seg(P2) {7}:bad'] }, { n: 'runs_（物理）', items: ['P2:P2 {7}:bad'] }], kv: { 'get(7)': '老值 ✗' }, log: ['!g9 t94c commit #30: key 7 resurrected'], ok: false },
  ],
  new: [
    { t: '同左初态。', rows: [{ n: '单元 C 的段', items: ['sR2:seg(R2) {7}:data', 'sR5:seg(R5) {7†}:tomb'] }, { n: 'runs_（物理）', items: ['R2:R2 {7}:data', 'R5:R5 {7†}:tomb'] }], kv: { 'get(7)': 'NOTFOUND ✓' } },
    { t: '单元压实同样摘掉 `seg(R2)`。', rows: [{ n: '单元 C 的段', items: ['sR5:seg(R5) {7†}:tomb'] }, { n: 'runs_（物理）', items: ['R2:R2 {7} 本体还在:dim', 'R5:R5 {7†}:tomb'] }] },
    { t: 'R5 参加层合并：`older_may_contain(7, R5)` 遍历 **runs_**：R2 更老、键域含 7、bloom 说可能 ⇒ **保留 7†**。', rows: [{ n: '单元 C 的段', items: ['P1:seg(P1) {7†}:tomb'] }, { n: 'runs_（物理）', items: ['R2:R2 {7} 本体还在:dim', 'P1:P1 {7†}:tomb'] }], kv: { 'older_may_contain(7)': 'true（R2.bloom）', '7†': '保留 ✓' }, ok: true },
    { t: '后续合并 {P1, R2}：7† 盖住 7 ⇒ 老值丢；此时下面再无更老 run ⇒ 7† 也丢。终局干净。', rows: [{ n: '单元 C 的段', items: ['P2:seg(P2) {}:good'] }, { n: 'runs_（物理）', items: ['P2:P2 {}:good'] }], kv: { 'get(7)': 'NOTFOUND ✓', '墓碑数': 0 }, ok: true },
  ],
});

PIT({
  id: 97, round: 11, cat: '范围墓碑 / 分步维护', found: 'g5 seed 3（R2 落地前）', tags: ['consolidate_step', '范围墓碑清除', 'rollback'],
  sym: '分步归一（`consolidate_step` 多次调用）做完后 `rollback`：被 `del_range` 盖住的键**复活**。一次性 `consolidate(0)` 没这个问题。',
  cause: '`consolidate(0)` 的语义是"全部归并一遍，再清范围墓碑"——墓碑能清是因为被盖住的记录已经在重写中物理消失。分步版的最后一步可能**一次归并都没做**（上一步之后新来了 `del_range`，而 run 数已经 ≤1），却照样执行了"收尾清墓碑"。',
  rule: '**范围墓碑只能在被它盖住的记录物理消失之后清除。**"物理消失" = 持有这些记录的 run 全部被重写过（且重写发生在墓碑之后）。',
  fix: '收尾步：只要 `ranges_` 非空就**强制重写**最后那条 run（单源 `merge_run_set(force=true)`），之后才清；只有"归并组恰好是全部 run"的那一步才顺手清。',
  why: '另一个方案是给每条墓碑记"覆盖的 run 集合，全部重写后再清"——账更精细但复杂；强制重写一次最后的 run 代价有限（分步归一本来就是全量重写）。',
  verify: 'g5 seed 3 回归；g9 分步归一段；`make test`。',
  cost: '收尾步多一次单源重写（最后那条 run 的大小）。',
  lesson: '把一个"原子操作"拆成多步时，每一步的前置条件都要重新证明，尤其是依赖"之前那一步刚做过什么"的收尾动作。',
  rel: [63, 78, 93],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `consolidate_step()` 收尾段（第 11 轮 R2）；老代码按 g5 seed 3 失败时的版本复原',
    old: `  RC consolidate_step() {
      if (runs_.size() <= 1) {
-         // 收尾：和 consolidate(0) 一样，清掉范围墓碑
!         ranges_.clear(); pend_ranges_.clear();    // 上一步之后若来了新 del_range，这里就清早了
          return RC::OK;
      }
      ... 合并最老的一组 ...
      return RC::MORE;
  }`,
    new: `  RC consolidate_step() {
      if (runs_.size() <= 1) {
+         // 坑 97：范围墓碑只能在被盖住的记录物理消失之后清。
+         // 最后一步可能一次归并都没做（上一步之后新来了 del_range）⇒ 有墓碑就强制重写最后那条 run
!         if (!ranges_.empty() && runs_.size() == 1) {
+             RC rc = merge_run_set({the_last_run}, delta, lvl, /*force=*/true, /*stream=*/false);
+             if (rc != RC::OK) return rc;
+         }
          ranges_.clear(); pend_ranges_.clear();
          return RC::OK;
      }
      ... 合并最老的一组；若组恰好 == 全部 run，顺手清墓碑 ...
      return RC::MORE;
  }`
  },
  old: [
    { t: '两条 run A、B，键 10 ~ 60。第 1 步：归并 {A,B} → P1（此时没有范围墓碑）。', lvl: { L0: ['A:A {10,20,30}:data', 'B:B {40,50,60}:data'] }, kv: { 'ranges_': '∅', 'rc': 'MORE' } },
    { t: '两步之间应用提交了 `del_range(0,50)` ⇒ 墓碑 G。P1 里 10/20/30/40 被盖住，但物理还在 P1 里。', lvl: { L0: ['P1:P1 {10,20,30,40,50,60}:data'] }, rows: [{ n: 'ranges_', items: ['G:G=[0,50)@v9:tomb'] }], kv: { 'get(20)': 'NOTFOUND ✓（靠 G）' } },
    { t: '第 2 步：`runs_.size()==1` ⇒ 收尾——**没做任何归并**就把 `ranges_` 清了。P1 原封不动。', lvl: { L0: ['P1:P1 {10,20,30,40,50,60}:bad'] }, rows: [{ n: 'ranges_', items: ['G:G 被清:del'] }], kv: { 'rc': 'OK', '被盖记录物理消失？': '否 ✗' }, ok: false },
    { t: '`rollback` / 重开：没有 G、P1 里还有 10 ~ 40 ⇒ **复活**。', lvl: { L0: ['P1:P1 {10,20,30,40,…}:bad'] }, kv: { 'get(20)': '找到 ✗' }, log: ['!g5 seed 3: key 20 visible after rollback (deleted by del_range)'], ok: false },
  ],
  new: [
    { t: '同左：第 1 步归并 {A,B} → P1。', lvl: { L0: ['A:A {10,20,30}:data', 'B:B {40,50,60}:data'] }, kv: { 'ranges_': '∅', 'rc': 'MORE' } },
    { t: '两步之间 `del_range(0,50)` ⇒ G。', lvl: { L0: ['P1:P1 {10,20,30,40,50,60}:data'] }, rows: [{ n: 'ranges_', items: ['G:G=[0,50)@v9:tomb'] }] },
    { t: '第 2 步收尾：`ranges_` 非空 ⇒ **强制重写** P1（单源 force）：被 G 盖住的 10~40 不 emit ⇒ P2 = {50,60}。', lvl: { L0: ['P2:P2 {50,60}:new'] }, rows: [{ n: 'ranges_', items: ['G:G=[0,50)@v9:tomb'] }], kv: { '被盖记录物理消失？': '是 ✓' } },
    { t: '现在才清 G。`rollback` / 重开：P2 里本来就没有 10~40，无需墓碑。', lvl: { L0: ['P2:P2 {50,60}:good'] }, rows: [{ n: 'ranges_', items: ['G:G 已清:del'] }], kv: { 'get(20)': 'NOTFOUND ✓' }, ok: true },
  ],
});

PIT({
  id: 98, round: 12, cat: '资源释放 / 工具链', found: '第 12 次讨论（新老同在）', tags: ['~Store', 'LSan', 'clear_state', 'detect_leaks'],
  sym: '反复 `open/close` 的进程每次 **+8 MB**（1M 条库）；ASan 目标只能 `detect_leaks=0` 跑——泄漏报告太多没法看。',
  cause: '`~Store()` 只停 I/O 线程；`runs_ / cells_`（含每条 run 的 bloom、稀疏索引、页表）、tls 槽、维护线程全靠进程退出收尸。嵌入式库被宿主反复挂载 / 卸载时就是真泄漏。',
  rule: '析构 = **先停所有会碰 `this` 的线程，再排空在飞 I/O，再释放结构**。顺序不能反：线程还活着就释放 `runs_`，线程下一次拿锁就是 UAF。',
  fix: '`~Store()`：`stop_maint_thread()`（mode 3）→ `set_async_writes(false)` / `wait_io(true)` → `clear_state()`。asan 目标常开 `detect_leaks=1`（之后每次新增的泄漏都会当场报）。',
  why: '不用智能指针全面重构：`Run` 的所有权已经集中在 `runs_`（RunPtr 是裸指针别名），`clear_state()` 本来就会 delete 它们——缺的只是析构时调用它。',
  verify: '`make asan` 全部段 `detect_leaks=1` 零报告（第 12 ~ 14 轮持续）；q1 反复 open/close RSS 不再爬。',
  cost: '析构多一次 `wait_io(true)`（排空在飞写），关库慢几 ms。',
  lesson: '泄漏检测关掉的那一天起，所有新泄漏都是免费的。工具链里的"临时关闭"要有归还日期。',
  rel: [109, 85],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `~Store()`；老代码按第 12 次讨论的描述复原',
    old: `  ~Store() {
-     stop_workers();          // 只停 I/O 线程
!     // runs_ / cells_ / tls 槽 … 全部不释放：靠进程退出收尸
  }
  // Makefile asan 目标：ASAN_OPTIONS=detect_leaks=0  ← 报告太多，干脆不看`,
    new: `  ~Store() {
+     stop_maint_thread();               // mode 3：先停后台维护线程（它会拿锁、碰设备）
+     if (async_) set_async_writes(false); else wait_io(true);   // 排空在飞写
!     clear_state();                     // 释放 runs_ / cells_ / bloom / 页表 / tls 槽
  }
  // Makefile asan 目标：ASAN_OPTIONS=detect_leaks=1  ← 常开，新泄漏当场报`
  },
  old: [
    { t: '宿主进程第 1 次 `open` 一个 1M 条的库：常驻结构 ≈ 8 MB（runs_ / cells_ / bloom / 页表）。', rows: [{ n: '堆', items: ['s1:会话 1 结构 8 MB:data'] }], kv: { 'RSS': '8 MB', 'I/O 线程': '运行' } },
    { t: '`~Store()`：只 `stop_workers()`。8 MB 结构没人 delete。', rows: [{ n: '堆', items: ['s1:会话 1 结构 8 MB（泄漏）:pend'] }], kv: { 'RSS': '8 MB', 'I/O 线程': '已停' } },
    { t: '第 2、3 次 open/close：每次 +8 MB。', rows: [{ n: '堆', items: ['s1:会话 1 8 MB:pend', 's2:会话 2 8 MB:pend', 's3:会话 3 8 MB:bad'] }], kv: { 'RSS': '24 MB ✗' }, ok: false },
    { t: 'LSan 报告几千条 ⇒ 干脆 `detect_leaks=0`；从此所有新泄漏都免费。', rows: [{ n: '堆', items: ['s1:8 MB:pend', 's2:8 MB:pend', 's3:8 MB:pend', 's4:…:bad'] }], kv: { 'detect_leaks': '0 ✗', '新泄漏': '看不见' }, log: ['!LeakSanitizer: 3 x 8 MB (runs_, cells_, bloom …)', '!→ ASAN_OPTIONS=detect_leaks=0'], ok: false },
  ],
  new: [
    { t: '第 1 次 open：同样 8 MB。', rows: [{ n: '堆', items: ['s1:会话 1 结构 8 MB:data'] }], kv: { 'RSS': '8 MB' } },
    { t: '`~Store()`：先停维护线程 → 排空在飞 I/O → `clear_state()` 释放全部结构。顺序保证线程不会碰到已释放的 `runs_`。', rows: [{ n: '堆', items: ['s1:已释放:del'] }], kv: { 'RSS': '≈0', '线程': '全部 join' }, lanes: [{ n: '析构', ev: [{ a: 0, b: 25, l: 'stop_maint', k: 'lock' }, { a: 25, b: 55, l: 'wait_io', k: 'io' }, { a: 55, b: 100, l: 'clear_state', k: 'ok' }] }], now: 100 },
    { t: '反复 open/close：RSS 平的。', rows: [{ n: '堆', items: ['s3:会话 3 结构 8 MB:data'] }], kv: { 'RSS': '8 MB ✓（不爬）' }, ok: true },
    { t: '`detect_leaks=1` 常开：第 12 ~ 14 轮 `make asan` 零报告，新泄漏当场抓。', kv: { 'detect_leaks': '1 ✓', 'LSan 报告': '0' }, log: ['+asan14.log: ASAN_EXIT=0, no LeakSanitizer output'], ok: true },
  ],
});

PIT({
  id: 99, round: 12, cat: '提交点 / 页复用', found: '第 12 轮 L1 落地时（g11 t_nospace；g5 种子 2 "page owned twice"）', tags: ['promote_pending', 'pend_need_gen_', 'sb_gen_', 'NOSPACE', 'commit_locked'],
  sym: 'NOSPACE 重试 / `abort()` 之后再崩溃：重开挂到的提交点引用的页里已经是**别的数据**（老提交点被覆盖）；另一现象：mode 1 下 g5 种子 2 报 "page owned twice"。',
  cause: '`promote_pending()` 把 `pending_free_` 放回 `free_` 的前提是"引用这些页的 DEL 已经随一个 SB 落盘"。老代码无条件放行，而调用点不全在 SB 之后：`commit()` 的 NOSPACE 重试路径在 `retire_dead_runs` 之后直接 promote，DEL（async 时连盖住它们的 ADD）都还没落盘就把页复用了。下一批 flush 写进去，崩溃后盘上 SB 仍引用这些页。',
  rule: '**页可以复用 ⇔ 盘上没有任何 SB 还引用它。** 这由"引用它的 DEL 已经被某个 SB 折叠"保证，所以需要一代计数：进 `pending_free_` 时记下 `pend_need_gen_ = sb_gen_ + 1`，只有 `sb_gen_` 真前进到那一代才放。',
  fix: '`promote_pending()`：`if (sb_gen_ < pend_need_gen_) return;`；NOSPACE 路径先写一个只含 DEL/ADD、不含范围墓碑的 SB（范围墓碑属于本批，不能先于本批的 put 落盘），再放页、再试。99b：重试再失败必须 `dirty_=true`；NOSPACE 的中间提交点不得持久化 `pend_ranges_`。同族：类内"抓状态当提交点"的路径（`snapshot()/abort()/maint_commit()`）必须走 `commit_locked(true)`，不能走公开 `commit()`——否则 mode 1 在持锁状态里再跑放锁的维护循环。',
  why: '不能用"每次 promote 前强制写 SB"——会把 NOSPACE 重试变成额外 fsync 风暴；闸只拦"还没过提交点"的情况，正常路径零开销。',
  verify: 'g11 `t_nospace`；g5 种子 2；g6 掉电注入；`make test` / `make asan`。',
  cost: '`pending_free_` 里的页在 NOSPACE 路径下多等一个 SB（多一次 `commit_delta`）。',
  lesson: '"提交点"是一个**事件**，不是一个函数名：任何推理"X 已经持久化"的代码都要指向具体哪一次 SB 写入把 X 折叠了进去。',
  rel: [5, 14, 104, 105],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `promote_pending()` 与 `commit()` NOSPACE 路径；老代码按第 12 轮 L1 落地前复原',
    old: `  void promote_pending() {
!     // 无条件放行：默认调用者一定在 SB 之后 —— 但 NOSPACE 重试 / abort / 提交后维护都不是
      for (auto& e : pending_free_) { erase...; free_insert(e); }
      pending_free_.clear();
  }
  // commit() NOSPACE 路径：
-     retire_dead_runs(delta);
!     promote_pending();            // DEL 还没落盘就把页放回 free_
      goto retry;                   // 下一批 flush 复用这些页 ⇒ 老提交点被覆盖`,
    new: `  void promote_pending() {
!     if (sb_gen_ < pend_need_gen_) return;   // 坑 99：有延迟页的 DEL 还没过提交点 ⇒ 整表继续等
      for (auto& e : pending_free_) { erase...; free_insert(e); }
      pending_free_.clear();
  }
  // commit() NOSPACE 路径：
+     // 坑 99：退休的 DEL（以及 async 刷出、盖住它们的 pend_delta_ ADD）必须先过一个提交点，页才能复用；
+     // 写一个**只含 DEL/ADD、不含范围墓碑**的 SB，然后再放页、再试。
+     if (!delta.empty() || !pend_delta_.empty()) {
+         if (!wait_io() || io_err_ != err0) { dirty_ = true; return RC::IO; }
+         RC r2 = commit_delta(delta, false);          // ++sb_gen_
+         if (r2 != RC::OK) { dirty_ = true; return r2; }
+     }
+     promote_pending();                                // 现在才放行`
  },
  old: [
    { t: '盘上 SB(gen 7) 引用 run R（页 p5）。R 退休 ⇒ p5 进 `pending_free_`，DEL 还只在内存 `delta` 里。', rows: [{ n: '盘上 SB(gen 7) 引用', items: ['p5:p5 ← R:data'] }, { n: 'pending_free_', items: ['pf5:p5:pend'] }, { n: '内存 delta', items: ['d:DEL R:tomb'] }], kv: { 'sb_gen_': 7 } },
    { t: 'flush 遇到 NOSPACE ⇒ 重试路径：`promote_pending()` **无条件**把 p5 放回 `free_`。', rows: [{ n: '盘上 SB(gen 7) 引用', items: ['p5:p5 ← R:data'] }, { n: 'free_', items: ['pf5:p5:free'] }, { n: '内存 delta', items: ['d:DEL R（未落盘）:tomb'] }], kv: { 'sb_gen_': 7, 'p5 可复用？': '放行 ✗' } },
    { t: '重试的 flush 把新 run N 写进 p5。盘上 SB 仍是 gen 7，仍说"p5 属于 R"。', rows: [{ n: '盘上 SB(gen 7) 引用', items: ['p5:p5 ← R（实际已是 N）:bad'] }, { n: '设备页 p5', items: ['n:N 的数据:new'] }], kv: { '盘上引用': 'R → p5', '页内容': 'N' } },
    { t: '此时掉电。重开挂到 SB(gen 7)：按 R 的描述去读 p5，读到 N 的页 ⇒ CRC / 键域不符 ⇒ 老提交点被毁。', rows: [{ n: '重开：REPLAY SB(gen 7)', items: ['p5:p5 ← R ?:bad'] }], kv: { 'verify': 'CORRUPT ✗' }, log: ['!mount: run R page p5 header mismatch (owner N)', '!g5 seed 2 (mode 1): page owned twice'], ok: false },
  ],
  new: [
    { t: '同左：R 退休 ⇒ p5 进 `pending_free_`，并记 `pend_need_gen_ = 8`（必须等 gen 8 的 SB）。', rows: [{ n: '盘上 SB(gen 7) 引用', items: ['p5:p5 ← R:data'] }, { n: 'pending_free_', items: ['pf5:p5:pend'] }, { n: '内存 delta', items: ['d:DEL R:tomb'] }], kv: { 'sb_gen_': 7, 'pend_need_gen_': 8 } },
    { t: 'NOSPACE 重试：`promote_pending()` 看 `sb_gen_(7) < pend_need_gen_(8)` ⇒ **拒绝放行**。', rows: [{ n: '盘上 SB(gen 7) 引用', items: ['p5:p5 ← R:data'] }, { n: 'pending_free_', items: ['pf5:p5（继续等）:pend'] }], kv: { '闸': '关 ✓' } },
    { t: '先写一个只含 DEL/ADD（不含本批范围墓碑）的 SB(gen 8)：盘上不再引用 p5。', rows: [{ n: '盘上 SB(gen 8) 引用', items: ['p5:p5 无主:hole'] }, { n: 'pending_free_', items: ['pf5:p5:pend'] }], kv: { 'sb_gen_': 8, 'pend_need_gen_': 8 }, lanes: [{ n: '提交', ev: [{ a: 0, b: 40, l: 'commit_delta(DEL R)', k: 'io' }, { a: 40, b: 55, l: 'fsync', k: 'io' }, { a: 55, b: 100, l: 'retry flush', k: 'ok' }] }], now: 55 },
    { t: '现在 `promote_pending()` 放行，重试 flush 复用 p5。掉电后无论挂到 gen 8 还是更新的 SB，都不会按 R 去读 p5。', rows: [{ n: 'free_ → 新 run N', items: ['p5:p5 ← N:new'] }], kv: { 'verify': 'OK ✓' }, log: ['+g11 t_nospace: PASS', '+99b: retry fail ⇒ dirty_=true; pend_ranges_ not persisted at NOSPACE SB'], ok: true },
  ],
});

PIT({
  id: 100, round: 12, cat: '维护进度 / 不变量 B1', found: '第 12 轮 M2 验收时（一次提交 50k 条）', tags: ['split_pass', 'split_pending_', 'BMAX', 'maint_unit MORE'],
  sym: '批量加载（一次提交 50k 条）后关库：一堆单元 > `BMAX`（32 KB 上限，不变量 B1），要到**下次维护**才补齐；期间读这些单元要扫超大段。',
  cause: '`split_pass` 每轮最多切 128 刀（有界单位，防提交尾巴过长）；切不完时它**不吭声**——返回值里没有"还有活"，`maint_unit` 以为维护做完了。',
  rule: '**有界步必须报告余量**：每个"最多做 N 件"的维护单位，做不完时必须让调度器知道（`MORE`），否则"有界"就变成了"丢活"。',
  fix: '`split_pass` 结束时 `split_pending_ = !work.empty()`；`maint_unit` 看到它就报 `MORE`，下一单元继续 `rebalance`。',
  why: '不把 128 刀上限去掉：一次提交里切几千刀会把提交尾巴拖到几百 ms（这正是 M2 分步化的目的）。',
  verify: 'g11 批量加载段：关库前所有单元 ≤ BMAX；重开 `verify` B1 不变量成立。',
  cost: '大批量加载后多几个维护单元（每单元 128 刀）。',
  lesson: '分步化之后，"做完了"必须是一个显式的、可观测的状态，不是"函数返回了"。',
  rel: [20, 101],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `split_pass()` 尾部与 `maint_unit()`；老代码按 M2 验收前复原',
    old: `  void split_pass(Delta& d) {
      std::vector<Cell*> work = 所有 bytes > BMAX 的单元;
      for (int i = 0; i < 128 && !work.empty(); ++i) { split(work.back()); work.pop_back(); }
!     // 切不完 ⇒ 静默返回：没人知道还有 172 个单元超限
  }
  RC maint_unit() { ... rebalance(d); ... return more ? RC::MORE : RC::OK; }   // more 不含 split 余量`,
    new: `  void split_pass(Delta& d) {
      std::vector<Cell*> work = 所有 bytes > BMAX 的单元;
      for (int i = 0; i < 128 && !work.empty(); ++i) { split(work.back()); work.pop_back(); }
!     split_pending_ = !work.empty();        // 坑 100：记下"还有没切完的"
  }
  RC maint_unit() {
      ...
      rc = rebalance(d);
      if (rc == RC::OK) retire_dead_runs(d);
+     if (split_pending_) more = true;       // 坑 100：128 刀没切完 ⇒ 还有活 ⇒ 报 MORE，下一单元继续
      ...
  }`
  },
  old: [
    { t: '一次提交 50k 条 ⇒ 300 个单元超过 BMAX。', rows: [{ n: '单元（示意，每格 = 50 个超限单元）', items: ['c1:50:bad', 'c2:50:bad', 'c3:50:bad', 'c4:50:bad', 'c5:50:bad', 'c6:50:bad'] }], kv: { '> BMAX 的单元': 300 } },
    { t: '`split_pass`：切 128 刀 ⇒ 还剩 172。函数静默返回。', rows: [{ n: '单元', items: ['c1:50:good', 'c2:50:good', 'c3:28/50:pend', 'c4:50:bad', 'c5:50:bad', 'c6:50:bad'] }], kv: { '> BMAX 的单元': 172, 'maint_unit 返回': 'OK（以为做完）' } },
    { t: '应用关库。172 个单元仍 > BMAX：B1 不变量在盘上被打破，读这些单元要扫超大段。', rows: [{ n: '盘上', items: ['c4:50:bad', 'c5:50:bad', 'c6:50:bad'] }], kv: { '关库时 > BMAX': '172 ✗', '下次修复': '下次维护才补' }, ok: false },
  ],
  new: [
    { t: '同左：300 个单元超限。', rows: [{ n: '单元', items: ['c1:50:bad', 'c2:50:bad', 'c3:50:bad', 'c4:50:bad', 'c5:50:bad', 'c6:50:bad'] }], kv: { '> BMAX 的单元': 300 } },
    { t: '第 1 个维护单元：切 128 刀，`split_pending_=true` ⇒ `maint_unit` 报 **MORE**。', rows: [{ n: '单元', items: ['c1:50:good', 'c2:50:good', 'c3:28/50:pend', 'c4:50:bad', 'c5:50:bad', 'c6:50:bad'] }], kv: { '> BMAX 的单元': 172, 'split_pending_': 'true', 'maint_unit 返回': 'MORE ✓' } },
    { t: '第 2、3 个单元继续切（128 + 44）⇒ 0。每个单元仍有界（≤128 刀），提交尾巴不变长。', rows: [{ n: '单元', items: ['c1:50:good', 'c2:50:good', 'c3:50:good', 'c4:50:good', 'c5:50:good', 'c6:50:good'] }], kv: { '> BMAX 的单元': '0 ✓', 'split_pending_': 'false' }, lanes: [{ n: '维护', ev: [{ a: 0, b: 33, l: '128 刀', k: 'run' }, { a: 33, b: 66, l: '128 刀', k: 'run' }, { a: 66, b: 78, l: '44 刀', k: 'ok' }] }], now: 78, ok: true },
  ],
});

PIT({
  id: 101, round: 12, cat: '维护模式 / 分步作业', found: '第 12 轮 M2 验收时', tags: ['mode 0', 'merge_job', 'guard', '切片', 'L0 堆积'],
  sym: 'mode 0（提交点之前做维护）下，一个 40 片的大合并被拖成 **3 次提交**才做完；期间新 run 堆到 1201 条，提交 avg 64 ms。',
  cause: '分步作业的 guard 每次提交只推进 ≤16 片——这个限制是为 mode 1（提交后、放锁切片让读者进来）设计的。mode 0 的维护在**同一把排他锁里、同一个提交点之前**，切片既放不了锁也挡不住后续提交，只是把活往后推、让 L0 堆积。',
  rule: '**切片只在"片与片之间能放锁 / 能有别的事发生"时才有意义**。同一临界区里把工作切片 = 纯延迟。',
  fix: 'mode 0（非 one 模式）不开分步作业；手头作业一口气推完。切片与预算属于 mode 1 / mode 3。',
  why: '不是给 mode 0 调大片数：根本上 mode 0 的语义就是"提交前把该做的做完"，合并大小由层规则控制，不该再有第二个节流阀。',
  verify: 'M2 验收：L0 run 数回到个位数，提交 avg 从 64 ms 回落；g11 mode 0 段。',
  cost: 'mode 0 下单次提交可能包含整个大合并（这正是 mode 0 的语义；要平滑用 mode 1/3）。',
  lesson: '节流机制要和它所在的并发上下文匹配：锁内节流只会制造积压。',
  rel: [100, 108],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` 维护入口的模式分支（第 12 轮 M2）；老代码按验收前复原',
    old: `  // 提交点之前（mode 0）也走分步作业：
- RC maint_before_commit(Delta& d) {
-     if (!job_) start_job(pick_set(), newlvl);
!     for (int i = 0; i < 16 && job_; ++i) merge_job_step(d);   // 每次提交最多 16 片（锁内！）
-     return RC::OK;                                            // 剩下的片下次提交再说 ⇒ L0 堆积
- }`,
    new: `  // mode 0（提交点之前）：不开作业，一口气合完 —— 同一把锁里切片没有意义
+ RC maint_before_commit(Delta& d) {
!     if (maint_mode_ == 0) return level_pass_whole(d);          // 整组合并，不切片
+     ...                                                        // mode 1 / 3 才 start_job + 切片 + 预算
+ }
  // 坑 101：mode 0（提交点之前）不开作业。第一版"每片独立产物、同 seq"也已否决（片与片之间老副本可见 ⇒ 复活）。`
  },
  old: [
    { t: 'mode 0：提交 #1 在排他锁内开作业 J（40 片），guard 只推 16 片就返回。', lanes: [{ n: '提交 #1', ev: [{ a: 0, b: 20, l: 'flush', k: 'io' }, { a: 20, b: 60, l: 'J 片 1-16', k: 'lock' }, { a: 60, b: 70, l: 'SB', k: 'io' }] }], now: 70, kv: { 'J 进度': '16/40', 'L0 run 数': 400, '提交耗时': '64 ms' } },
    { t: '提交 #2：再推 16 片。新来的 run 一直在 L0 堆。', lanes: [{ n: '提交 #2', ev: [{ a: 0, b: 20, l: 'flush', k: 'io' }, { a: 20, b: 60, l: 'J 片 17-32', k: 'lock' }, { a: 60, b: 70, l: 'SB', k: 'io' }] }], now: 70, kv: { 'J 进度': '32/40', 'L0 run 数': 800 } },
    { t: '提交 #3 才做完。期间 L0 堆到 1201 条——读路径要查 1201 条 run 的 bloom。', lanes: [{ n: '提交 #3', ev: [{ a: 0, b: 20, l: 'flush', k: 'io' }, { a: 20, b: 40, l: 'J 片 33-40', k: 'lock' }, { a: 40, b: 50, l: 'SB', k: 'io' }] }], now: 50, kv: { 'J 进度': '40/40', 'L0 run 数': '1201 ✗', '提交 avg': '64 ms ✗' }, ok: false },
  ],
  new: [
    { t: 'mode 0：提交 #1 在锁内**一口气**合完整组（40 片的工作量一次做）。', lanes: [{ n: '提交 #1', ev: [{ a: 0, b: 15, l: 'flush', k: 'io' }, { a: 15, b: 85, l: '整组合并', k: 'lock' }, { a: 85, b: 95, l: 'SB', k: 'io' }] }], now: 95, kv: { 'L0 run 数': 8, '本次提交': '较长（语义如此）' } },
    { t: '提交 #2、#3 轻快：L0 没有积压。总工作量相同，只是不再把活往后推。', lanes: [{ n: '提交 #2', ev: [{ a: 0, b: 15, l: 'flush', k: 'io' }, { a: 15, b: 25, l: 'SB', k: 'io' }] }, { n: '提交 #3', ev: [{ a: 0, b: 15, l: 'flush', k: 'io' }, { a: 15, b: 25, l: 'SB', k: 'io' }] }], now: 25, kv: { 'L0 run 数': '8 ✓', '提交 avg': '回落 ✓' }, ok: true },
    { t: '要"平滑"就换 mode 1（提交后、放锁切片、墙钟预算）或 mode 3（后台线程）——切片在那里才有意义。', kv: { 'mode 0': '提交前做完', 'mode 1': '提交后 + 预算', 'mode 3': '后台线程 + 让路' }, ok: true },
  ],
});
