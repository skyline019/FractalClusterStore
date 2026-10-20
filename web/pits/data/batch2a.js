// 第 2 批（a）：坑 1 ~ 18（v1 / v2 基础期）。第 10 轮之前没有历史头：代码块一律"示意复原"（按 DESIGN3 §12 与当前头复原关键行），
// 能从当前 include/fcs.h 原样引用的新代码标 half（新真实 / 老复原）。

PIT({
  id: 1, round: 'v1', cat: '结构 / 段与单元', found: 'v1 切分 + 回并测试', tags: ['split', 'Seg', '键域', '幽灵段'],
  sym: '切分之后再合并，同一条记录被**计两次**：`verify` 的 live 比真值大，压实后产物里出现重复副本。',
  cause: '`split()` 给两个子单元各复制一份父单元的段表，其中包括"该 run 在本子区间**没有**记录"的段——一个空壳。它的键域没被收窄，日后合并回来（或 recount）时这个空壳又把邻居区间的记录圈了进来。',
  rule: '**段 = (run, 键域, 页区间)**，键域必须非空且落在单元键域内；"没有记录"的段不是段，是幽灵。',
  fix: '切分时按 run 的 fence 算出每个子区间真正覆盖的键域；键域为空的段**不保留**。',
  why: '不靠"读时去重"：去重要在每次读路径上做比较，成本在热路径；而且重复副本会让存活账虚高，影响压实决策。从源头不制造幽灵最便宜。',
  verify: 'v1 切分测试：切分 → 合并 → `verify` live 与 recount 一致；压实产物无重复。',
  cost: '切分时多一次 fence 二分（每段 O(log 页数)）。',
  lesson: '复制结构时，每个复制品都要重新满足它自己的不变量，不能"先复制再说"。',
  rel: [2, 20, 35],
  code: { kind: 'sketch', src: '按 §12 第 1 条复原；当前 `split()` 见 `include/fcs.h`',
    old: `  void split(Cell* c, u64 mid) {
      Cell* l = new Cell{c->lo, mid}; Cell* r = new Cell{mid, c->hi};
-     l->segs = c->segs;  r->segs = c->segs;        // 两边各一份完整段表
!     // 某条 run 在 [mid, hi) 里一条记录都没有 ⇒ r 里留下一个"幽灵段"，键域仍是父单元的
  }`,
    new: `  void split(Cell* c, u64 mid) {
      Cell* l = new Cell{c->lo, mid}; Cell* r = new Cell{mid, c->hi};
      for (auto& [k, sg] : c->segs) {
+         auto [klo, khi] = sg_keys_within(sg, c->lo, mid);    // 用 run 的 fence 算真正覆盖的键域
!         if (klo < khi) l->segs.emplace(k, Seg{sg.page0, p0, p1, klo, khi});   // 键域非空才保留
+         auto [klo2, khi2] = sg_keys_within(sg, mid, c->hi);
!         if (klo2 < khi2) r->segs.emplace(k, Seg{...});
      }
      retighten(l); retighten(r);                             // 坑 20：页区间同步收紧
  }`
  },
  old: [
    { t: '单元 C=[0,100) 有两段：run A 的记录全在 [0,50)，run B 的记录全在 [50,100)。', rows: [{ n: '单元 C [0,100)', items: ['A:seg(A) 键 10,20,30:data', 'B:seg(B) 键 60,70:data'] }], kv: { 'live': 5 } },
    { t: '`split(C, 50)`：两个子单元各复制一份段表。R=[50,100) 里的 seg(A) 一条记录都没有——**幽灵段**，键域还是 [0,100)。', rows: [{ n: 'L [0,50)', items: ['LA:seg(A) 10,20,30:data', 'LB:seg(B) 空壳:bad'] }, { n: 'R [50,100)', items: ['RA:seg(A) 空壳:bad', 'RB:seg(B) 60,70:data'] }], kv: { 'L.live': 3, 'R.live': 2 } },
    { t: '日后 recount / 合并：R 的幽灵 seg(A) 键域没收窄 ⇒ 把 A 的 10,20,30 **又圈进来**。', rows: [{ n: 'R [50,100) recount', items: ['RA:seg(A) → 10,20,30 ?!:bad', 'RB:seg(B) 60,70:data'] }], kv: { 'R.live': '5 ✗（真值 2）', '全库 live': '8 ✗（真值 5）' }, log: ['!verify: cell R live 5 vs recount-by-keys 2'], ok: false },
  ],
  new: [
    { t: '同左初态。', rows: [{ n: '单元 C [0,100)', items: ['A:seg(A) 键 10,20,30:data', 'B:seg(B) 键 60,70:data'] }], kv: { 'live': 5 } },
    { t: '`split(C, 50)`：对每段用 run 的 fence 算真正覆盖的键域；空的**不保留**。', rows: [{ n: 'L [0,50)', items: ['LA:seg(A) [10,30]:good'] }, { n: 'R [50,100)', items: ['RB:seg(B) [60,70]:good'] }], kv: { 'L.live': 3, 'R.live': 2 } },
    { t: 'recount / 合并只看非空段 ⇒ 账与真值一致，没有重复副本。', kv: { '全库 live': '5 ✓' }, ok: true },
  ],
});

PIT({
  id: 2, round: 'v1', cat: '结构 / 段与单元', found: 'v1 单元回并实验', tags: ['cell merge', '只细分不回并', '重复副本'],
  sym: '两个相邻小单元回并成一个之后，压实出来的产物里有**重复副本**，live 虚高。',
  cause: '回并把邻居单元的段重新纳入本单元视野；而本单元之前的压实产物（只含本区间的赢家）和邻居的段里同一个键的版本**同时可见**，归并把它们当成两条记录。',
  rule: '**单元只细分，不回并。**单元边界是单调加细的，段的归属因此永不回流；小单元的代价（多几个结构）远小于回并的正确性风险。',
  fix: '删除回并路径；小单元交给"压实时整单元重写"自然收敛。',
  why: '要安全回并必须保证两边的段集合对同一个键只有一个赢家来源，等价于先做一次跨单元归并——那就是重写，不如直接不回并。',
  verify: 'v1 压实测试无重复；后续所有轮次保持"只细分"。',
  cost: '长期删除负载下可能留下很多小单元（空单元在压实时会消失，不是问题）。',
  lesson: '让某个结构的演化**单调**（只分不合、只增不减）能消掉一整类状态机 bug。',
  rel: [1, 8],
  code: { kind: 'sketch', src: '按 §12 第 2 条复原',
    old: `- void merge_cells(Cell* a, Cell* b) {     // 相邻小单元回并
-     a->hi = b->hi;
!     for (auto& s : b->segs) a->segs.insert(s);   // 邻居的段重新纳入视野：与 a 的压实产物形成重复副本
-     delete b;
- }`,
    new: `+ // 单元只细分不回并：没有 merge_cells。
+ // 小单元由 compact_cell 整单元重写收敛；空单元在压实时自然消失。
  void split(Cell* c, u64 mid) { ... }`
  },
  old: [
    { t: '单元 A=[0,50) 已压实：产物 P 含键 10 的最新值 v2。邻居 B=[50,100) 的段里还挂着老 run R（R 跨两个区间，含 10@v1 与 60）。', rows: [{ n: 'A [0,50)', items: ['P:seg(P) 10@v2:new'] }, { n: 'B [50,100)', items: ['R:seg(R) 60（R 里还有 10@v1）:data'] }] },
    { t: '回并 A+B：R 的段重新纳入 [0,100) 视野 ⇒ 10@v1 与 10@v2 **同时可见**。', rows: [{ n: 'AB [0,100)', items: ['P:seg(P) 10@v2:new', 'R:seg(R) 10@v1, 60:bad'] }], kv: { 'live': '3 ✗（真值 2）' } },
    { t: '压实归并把两条 10 当成两条记录（不同 run 不同 seq，胜者规则本可处理，但账与段口径已乱）⇒ 产物重复、live 虚高。', kv: { 'verify': 'live mismatch ✗' }, ok: false },
  ],
  new: [
    { t: '同左。', rows: [{ n: 'A [0,50)', items: ['P:seg(P) 10@v2:new'] }, { n: 'B [50,100)', items: ['R:seg(R) 60:data'] }] },
    { t: '**不回并。**A 与 B 各自演化：B 压实时 R 的 60 进 B 的产物，R 退休。', rows: [{ n: 'A [0,50)', items: ['P:seg(P) 10@v2:good'] }, { n: 'B [50,100)', items: ['Q:seg(Q) 60:good'] }], kv: { 'live': '2 ✓' }, ok: true },
  ],
});

PIT({
  id: 3, round: 'v1', cat: '结构 / seq 全序', found: 'v1 压实后读到旧值', tags: ['seq', 'seg map', '撞号', '(cell, seq, rid)'],
  sym: '压实之后 `get` 返回**旧值**；`verify` 看段表里少了一条段。',
  cause: '压实产物的 seq 与它替换掉的某条 run **撞号**：段表以 seq 为键，一条覆盖另一条，剩下的那条可能指向旧数据。',
  rule: '**段身份用 (cell, seq, rid) 全序，seq 全局唯一**；压实 / 合并产物的 seq 必须让它在读路径的全序里落到正确位置（v1：取源的最大 seq，"栈底"；第 10 轮后：压实产物取新鲜 seq 进 L0，见坑 90）。',
  fix: '`seg_key(seq, rid)` 作段表键；产物 seq 全局分配，不与任何活 run 相同。',
  why: '不用"替换前先删后插"掩盖：撞号的根源是 seq 不唯一，掩盖只会把问题推到读路径的胜者判定。',
  verify: 'v1 压实回归；后续 seqwrap 探针（坑 103）守着唯一性。',
  cost: '无。',
  lesson: '用作 map 键的"身份"必须和对象的生命周期一样长、一样唯一。',
  rel: [4, 9, 16, 36, 90],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `seg_key()`；老代码按 §12 第 3 条复原',
    old: `- std::map<u32, Seg> segs;                      // 键 = seq
- ...
- Run* nr = compact(c);  nr->seq = some_src->seq;   // 产物沿用某条源的 seq
! c->segs[nr->seq] = Seg{...};                      // 与被替换的 run 撞号 ⇒ 一条覆盖另一条`,
    new: `+ inline u64 seg_key(u32 seq, u32 rid) { return ((u64)seq << 32) | rid; }   // (seq, rid) 全序
  std::map<u64, Seg> segs;                                                     // 键 = seg_key
  ...
! nr->seq = fresh_or_max_src_seq();                 // 全局唯一：不与任何活 run 相同
  c->segs.emplace(seg_key(nr->seq, nr->page0), Seg{...});`
  },
  old: [
    { t: '单元里有 run R1(seq 5) 与 R2(seq 7)；键 k 的最新值在 R2。', rows: [{ n: 'segs（键 = seq）', items: ['s5:seq5 → R1 {k@v1}:data', 's7:seq7 → R2 {k@v2}:data'] }], kv: { 'get(k)': 'v2 ✓' } },
    { t: '压实产物 P 沿用 seq 5（源 R1 的）：`segs[5] = P` **覆盖**了 R1 的段；R2 的段还在，但 P 已含 k@v2……顺序却按 seq 5 排在 R2 之前。', rows: [{ n: 'segs', items: ['s5:seq5 → P {k@v2}:bad', 's7:seq7 → R2 {k@v2}（本应退休）:data'] }], kv: { '账': '乱' } },
    { t: '另一种撞法：P 拿到的 seq 与仍活着的 R2 相同 ⇒ `segs[7]` 被覆盖，R2 的别的键消失 ⇒ `get` 旧值 / NOTFOUND。', rows: [{ n: 'segs', items: ['s7:seq7 → P（R2 的段没了）:bad'] }], kv: { 'get(其它键)': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '段表键 = `seg_key(seq, rid)`，seq 全局唯一分配。', rows: [{ n: 'segs（键 = (seq,rid)）', items: ['s5:(5,r1) → R1:data', 's7:(7,r2) → R2:data'] }], kv: { 'get(k)': 'v2 ✓' } },
    { t: '压实产物 P 取新 seq 9（第 10 轮后：新鲜 seq 进 L0）：插入不覆盖任何段；源段随后显式删除。', rows: [{ n: 'segs', items: ['s9:(9,p) → P {k@v2}:new'] }], kv: { 'get(k)': 'v2 ✓' }, ok: true },
  ],
});

PIT({
  id: 4, round: 'v1 / v2', cat: '清单 / 段身份', found: 'v1；组提交 + 分层合并引入后再次咬人', tags: ['M_SEG_DEL', '页号复用', '(cell, rid, seq)', 'replay'],
  sym: '重开后某条**新** run 的段不见了：清单重放把它删掉了。',
  cause: '页号会复用：run 退休后它的首页 `rid` 很快被新 run 拿到。清单里 `M_SEG_DEL` 若只带 `(cell, rid)`，重放时会匹配到**刚复用同一页号的新 run** 的段。组提交 / 分层合并让"退休 → 复用"在同一个清单窗口里发生，于是再次咬人。',
  rule: '**段身份 = (cell, rid, seq)**，清单里的删除项必须写全三元组；任何按身份查找的地方都用全序键。',
  fix: '`M_SEG_DEL` 带 seq；重放按 `seg_key(seq, rid)` 精确匹配。',
  why: '不靠"延迟复用页号"：页号复用是空间回收的本质，推迟只是缩小窗口不是消灭。',
  verify: 'v2 组提交 + 合并 + 重开回归；g 系列 reopen 段持续覆盖。',
  cost: '清单删除项多 4 字节。',
  lesson: '任何会被回收复用的标识符（页号、槽位、fd）都不能单独当身份用，要配一个单调序号。',
  rel: [3, 80, 14],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` 枚举 @120、`ManEnt` @2922、重放 @3841、写项 @6678；老代码按 §12 第 4 条复原',
    old: `- struct ManEnt { u8 kind; u64 cell; u32 rid; };          // M_SEG_DEL 只带 (cell, rid)
  // replay：
- case M_SEG_DEL: cells_[m.cell]->segs.erase_by_rid(m.rid);   // 页号已被新 run 复用 ⇒ 删错段`,
    new: `  enum : u8 { M_RUN_ADD = 1, M_RUN_DEL = 2, M_SPLIT_ADD = 3, M_SPLIT_DEL = 4, M_SEG_DEL = 5, M_RANGE = 6 };
  struct ManEnt { u8 op; u8 lvl; u64 key, last; u32 page0, npages, ver, nbytes, seq; };   // seq 进了清单项
  // 写删除项（compact_cell / 摘段处）：
  ManEnt m; m.op = M_SEG_DEL; m.key = c->lo; m.page0 = rid;
! m.seq = rr ? rr->seq : 0;                                  // (cell=key, rid=page0, seq) 三元组写全
  // 重放（apply_man）：
! segdels_.push_back({m.key, seg_key(m.seq, m.page0)});     // 等 attach_run 之后按 (seq,rid) 精确摘段
  // 自证（坑 26）：if (m.op == M_SEG_DEL) return m.page0 >= 2 && m.seq != 0;`
  },
  old: [
    { t: 'run A（首页 rid=40, seq 3）退休：清单记 `SEG_DEL(cell, rid=40)`。页 40 回到空闲表。', rows: [{ n: '清单', items: ['d:SEG_DEL cell,rid=40:tomb'] }, { n: '页 40', items: ['p:空闲:free'] }] },
    { t: '同一清单窗口里新 run B 分配到 **rid=40**（seq 9），清单记 `RUN_ADD B`。', rows: [{ n: '清单', items: ['d:SEG_DEL cell,rid=40:tomb', 'a:RUN_ADD B rid=40 seq=9:new'] }, { n: '页 40', items: ['p:B 的数据:new'] }] },
    { t: '重开重放：先 ADD B 挂段（按清单顺序可能先后不同），再遇到 `SEG_DEL rid=40` ⇒ 按 rid 匹配 ⇒ **删掉了 B 的段**。', rows: [{ n: '重放后 segs', items: ['b:seg(B) 被删:bad'] }], kv: { 'get(B 的键)': 'NOTFOUND ✗' }, log: ['!replay: M_SEG_DEL rid=40 matched new run B (seq 9)'], ok: false },
  ],
  new: [
    { t: 'A 退休：清单记 `SEG_DEL(cell, rid=40, seq=3)`。', rows: [{ n: '清单', items: ['d:SEG_DEL rid=40 seq=3:tomb'] }] },
    { t: 'B 复用 rid=40（seq 9）。', rows: [{ n: '清单', items: ['d:SEG_DEL rid=40 seq=3:tomb', 'a:RUN_ADD B rid=40 seq=9:new'] }] },
    { t: '重放：`SEG_DEL` 按 `seg_key(3, 40)` 精确匹配，B 的段键是 `seg_key(9, 40)` ⇒ 不受影响。', rows: [{ n: '重放后 segs', items: ['b:seg(B) (9,40):good'] }], kv: { 'get(B 的键)': '找到 ✓' }, ok: true },
  ],
});

PIT({
  id: 5, round: 'v1', cat: '提交点 / 页复用', found: 'v1 掉电注入', tags: ['pending_free_', '完整旧态', '提交失败'],
  sym: '提交途中失败 / 掉电后重开，上一个"完整旧态"也坏了：旧提交点引用的页里是半截新数据。',
  cause: '本次提交里退休的 run 的页被**立刻**放回空闲表，同一次提交的后续 flush 就复用了它们；提交最终没成功（或掉电），盘上 SB 仍是旧的、仍引用这些页。',
  rule: '**提交点之前释放的页，要到提交点之后才可复用**（两阶段释放）。',
  fix: '引入 `pending_free_`：退休的页先进这里；`write_sb` 成功后 `promote_pending()` 才移入 `free_`。',
  why: '不靠"失败时回滚空闲表"：掉电没有机会回滚；两阶段释放对掉电和失败一视同仁。',
  verify: 'v1 掉电注入；后续 g6 掉电注入全部种子。',
  cost: '一个提交周期内空闲页晚一步可用（NOSPACE 边界略紧，坑 23 处理）。',
  lesson: '"释放"在有持久状态的系统里是两步：逻辑释放 + 持久确认。',
  rel: [14, 99, 104, 33],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `promote_pending()`（约 4048 行）；老代码按 §12 第 5 条复原',
    old: `  void retire(Run* r) {
!     free_insert(r->page0, r->npages);     // 立刻可复用 —— 本次提交的后续 flush 就会拿到它
      runs_.erase(r->seq);
  }`,
    new: `  void retire(Run* r, Delta& d) {
!     pending_free_.push_back({r->npages, r->page0});   // 先挂起
      d.del.push_back(...); runs_.erase(...);
  }
  RC commit_locked() {
      ... flush → commit_delta → write_sb() ...         // 提交点
+     promote_pending();                                // 坑 5：提交点之后才进 free_
  }
  void promote_pending() {
      // 提交点已落盘 ⇒ 这些页不会再被上一版状态引用：先擦除（NOR 只能 1→0），再放回空闲表
!     if (sb_gen_ < pend_need_gen_) return;             // 坑 99 的闸：自上次 free 之后必须写过 SB
      for (auto& e : pending_free_) { ... free_[e.second] = e.first; }
  }`
  },
  old: [
    { t: '盘上 SB(v7) 引用 run R（页 20~22）。本次提交里 R 退休 ⇒ 页 20~22 **立刻**回 free_。', rows: [{ n: '盘上 SB(v7) 引用', items: ['R:R 页20-22:data'] }, { n: 'free_', items: ['f:20-22:free'] }] },
    { t: '同一次提交的 flush 把新 run N 写进 20~22。', rows: [{ n: '盘上 SB(v7) 引用', items: ['R:R 页20-22（内容已是 N）:bad'] }, { n: '设备 20-22', items: ['n:N 的页:new'] }] },
    { t: '提交失败 / 掉电 ⇒ 重开挂 SB(v7)：按 R 读 20~22，读到 N 的半截页 ⇒ 完整旧态已被破坏。', kv: { 'REPLAY R': 'CRC 不符 ✗' }, log: ['!mount: run R page 20 crc mismatch'], ok: false },
  ],
  new: [
    { t: 'R 退休 ⇒ 页 20~22 进 `pending_free_`（不可分配）。', rows: [{ n: '盘上 SB(v7) 引用', items: ['R:R 页20-22:data'] }, { n: 'pending_free_', items: ['pf:20-22:pend'] }] },
    { t: '本次 flush 只能用别的空闲页 / 长尾；提交失败或掉电时 20~22 原样未动 ⇒ 旧态完整。', rows: [{ n: '盘上 SB(v7) 引用', items: ['R:R 页20-22 原样:good'] }, { n: '新 run N', items: ['n:N 页30-32:new'] }], ok: true },
    { t: 'SB(v8) 落盘后 `promote_pending()` ⇒ 20~22 进 free_。', rows: [{ n: 'free_', items: ['f:20-22:free'] }], kv: { '可复用': '提交点之后 ✓' }, ok: true },
  ],
});

PIT({
  id: 6, round: 'v1', cat: '介质 / NOR', found: 'v1 NOR 模拟设备', tags: ['NOR', '1→0', 'erase', '提交点'],
  sym: 'NOR 介质上覆盖写一页：写进去的字节是"新 & 旧"的按位与——数据坏，CRC 报错。',
  cause: 'NOR 只能把位从 1 写成 0，想得到 1 必须整块擦除。直接在已写过的页上写新内容，位只能"更 0"。',
  rule: '**先擦后写；且擦除必须发生在提交点之后**（擦除是破坏性的：擦掉的页若仍被盘上 SB 引用，掉电后旧态就坏了——和坑 5 同一条原则）。',
  fix: '`promote_pending()` 放行页时对每页 `dev_->erase()`（它在提交点之后），分配到的页因此总是已擦除状态。',
  why: '不在分配时擦：分配发生在提交点之前，擦掉的可能是旧 SB 仍引用的页。',
  verify: 'v1 NOR 设备模型测试；`RamDev::valid` 的 1→0 检查。',
  cost: '提交点后多一次擦除批。',
  lesson: '介质约束要进入状态机（什么时候允许擦），不能只在驱动层"顺手处理"。',
  rel: [5, 12],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `promote_pending()`（约 4048 行）；老代码按 §12 第 6 条复原',
    old: `- // 直接覆盖写：NOR 上 1 写不回来 ⇒ 页内容 = 新 & 旧
- dev_->write(pid, buf);`,
    new: `  void promote_pending() {
      // 提交点已落盘 ⇒ 这些页不会再被上一版状态引用：先擦除（NOR 只能 1→0），
      // 再放回空闲表，避免复用被擦写语义污染。
      if (sb_gen_ < pend_need_gen_) return;                 // 坑 99 的闸：自上次 free 之后必须写过 SB
      for (auto& e : pending_free_) {
          if (e.second >= tail_ || (u64)e.second + e.first > (u64)tail_) continue;
!         if (dev_) for (u32 i = 0; i < e.first; ++i) dev_->erase((PageId)(e.second + i));   // 提交点之后擦
          if (dev_ && discard_min_ && e.first >= discard_min_) dev_->discard((PageId)e.second, e.first);  // R8 局部 TRIM
          free_[e.second] = e.first;
      }
  }`
  },
  old: [
    { t: '页 p 上有旧数据（位 1011…）。直接写新内容（位 1101…）。', rows: [{ n: '页 p', items: ['o:旧 1011:data', 'n:写入 1101:new'] }] },
    { t: 'NOR 只能 1→0 ⇒ 实际落盘 1011 & 1101 = **1001**。CRC 不符。', rows: [{ n: '页 p', items: ['r:1001 ✗:bad'] }], kv: { 'CRC': '✗' }, ok: false },
  ],
  new: [
    { t: '页 p 退休 → `pending_free_` → 提交点 → `promote_pending()` 里 `erase(p)`：全 1。', rows: [{ n: '页 p', items: ['e:1111（已擦）:free'] }] },
    { t: '再分配给新 run，写 1101 ⇒ 1111 & 1101 = 1101 ✓。擦除在提交点之后，旧 SB 引用的页不会被擦。', rows: [{ n: '页 p', items: ['r:1101 ✓:good'] }], ok: true },
  ],
});

PIT({
  id: 7, round: 'v1', cat: '快照 / 回收', found: 'v1 快照测试', tags: ['snapshot', '压实', '互斥'],
  sym: '持有快照时做压实 / 回收，快照视图里的记录**消失**或读到新值。',
  cause: '压实只保留最新版本并释放源 run；快照需要的旧版本正在源 run 里。',
  rule: '**有快照时暂停回收**（v1）；后续精细化为"只回收 ver > snap_max 的 run / 只在 snap 门禁下压实"（P5、坑 69、91）。',
  fix: 'v1：`nsnap_ > 0` ⇒ 压实 / 退休一律跳过；页释放延后。',
  why: '最简单正确的版本先落地；精细门禁在后续轮次（P5）做。',
  verify: 'v1 快照回归；后续 g7 / g9 快照段。',
  cost: '快照期间垃圾累积。',
  lesson: '多版本系统里"谁还需要这个版本"是回收的唯一合法判据；拿不准就不收。',
  rel: [69, 91, 93],
  code: { kind: 'sketch', src: '按 §12 第 7 条复原（v1）',
    old: `  void compact_pass() {
!     for (Cell* c : victims) compact_cell(c);       // 不看快照：源 run 释放，快照需要的旧版本没了
  }`,
    new: `  void compact_pass() {
!     if (nsnap_) return;                            // v1：有快照时暂停回收
      for (Cell* c : victims) compact_cell(c);
  }
  // 后续：组只含 ver > snap_max() 的 run（坑 69 / 91），退休门禁看 snap`
  },
  old: [
    { t: '快照 S@v5 持有；键 k 有 v3（run R1）与 v8（run R2）。S 应读到 v3。', rows: [{ n: 'runs', items: ['R1:R1 k@v3:data', 'R2:R2 k@v8:data', 'S:快照 S@v5:lock'] }], kv: { 'get(k, S)': 'v3 ✓' } },
    { t: '压实：只保留最新版本 ⇒ 产物 P{k@v8}，R1 释放。', rows: [{ n: 'runs', items: ['P:P k@v8:new', 'S:快照 S@v5:lock'] }], kv: { 'get(k, S)': 'v8 ✗（或 NOTFOUND）' }, ok: false },
  ],
  new: [
    { t: '`nsnap_ > 0` ⇒ 本轮压实跳过；R1 保留。', rows: [{ n: 'runs', items: ['R1:R1 k@v3:data', 'R2:R2 k@v8:data', 'S:快照 S@v5:lock'] }], kv: { 'get(k, S)': 'v3 ✓' }, ok: true },
    { t: 'S 释放后再压实。后续轮次精细化：组只含 `ver > snap_max` 的 run，快照期也能回收不相关的垃圾。', rows: [{ n: 'runs', items: ['P:P k@v8:good'] }], ok: true },
  ],
});

PIT({
  id: 8, round: 'v1', cat: '压实策略', found: 'v1 空间不回收', tags: ['前缀合并', '整 cell 重写', '最新版本'],
  sym: '"前缀合并"（只把单元里最老的几段合一合）做了很多次，空间一点不回收。',
  cause: '压实只保留最新版本；旧版本能不能丢取决于**更新的段里有没有同键**。只合并老段看不到新段，所有旧版本都得保留 ⇒ 什么也回收不了。',
  rule: '**单元级回收 = 整单元重写**（k 路归并全部段），让每个键的赢家一次判定；部分段的合并只能整理结构，不能回收空间。',
  fix: '`compact_cell` 整单元重写；"值不值得"用 dead/live 性价比（`FCS_GC_RATIO`）判。',
  why: '层合并（LSM）处理的是跨单元的大 run 组，单元压实处理单元内垃圾；两者分工后不再做前缀合并。',
  verify: 'v1 回收测试：覆盖负载空间收敛。',
  cost: '整单元重写写放大更高（单元 ≤ BMAX=32 KB，每次重写量有界）。',
  lesson: '判定"可丢"需要看到全部竞争者；局部视图下的回收永远是保守的。',
  rel: [2, 27, 94],
  code: { kind: 'sketch', src: '按 §12 第 8 条复原',
    old: `- // 前缀合并：只合单元里最老的 n 段
- merge(segs.begin(), segs.begin() + n);     // 看不到更新的段 ⇒ 旧版本都得留 ⇒ 回收 0`,
    new: `  RC compact_cell(Cell* c, Delta& d) {
!     k_way_merge(c->segs.begin(), c->segs.end(), emit_winner_only);   // 整单元：每个键只留赢家
      ... 产物挂段、源段摘除、live/dead 重算 ...
  }`
  },
  old: [
    { t: '单元有 4 段：S1{k@v1} S2{k@v2} S3{j} S4{k@v4}。前缀合并 S1+S2。', lvl: { '段': ['S1:S1 k@v1:data', 'S2:S2 k@v2:data', 'S3:S3 j:data', 'S4:S4 k@v4:data'] }, kv: { 'dead': 2 } },
    { t: '合并 S1+S2 时看不到 S4：k@v2 是"目前所见最新" ⇒ 保留。回收 = 1 条（v1），下一次又是前缀……空间不收敛。', lvl: { '段': ['P:P k@v2:bad', 'S3:S3 j:data', 'S4:S4 k@v4:data'] }, kv: { 'dead': '1 → 永远 ≥1 ✗' }, ok: false },
  ],
  new: [
    { t: '整单元 k 路归并：k 的赢家 = v4，v1/v2 全丢。', lvl: { '段': ['P:P j, k@v4:good'] }, kv: { 'dead': '0 ✓' }, ok: true },
  ],
});

PIT({
  id: 9, round: 'v2', cat: '分层合并 / seq', found: 'v2 分层合并（§5.3）', tags: ['merge', 'seq 继承', '连续性', '坑 90/91'],
  sym: '分层合并之后旧值**复活**。',
  cause: '合并产物如果拿一个比源里最新 seq 更小的 seq（例如最老源的），读路径的 `(seq, rid)` 全序会把它排在某条未参加合并的中间 run 之后——那条 run 里的老值就赢了。',
  rule: '合并产物继承**源里最新的 seq**；而且合并组必须在全库 seq 顺序上**连续**（坑 91），否则组内最新 seq 仍可能被跨过的中间 run 盖住。单元压实产物例外：取新鲜 seq、进 L0（坑 90）。',
  fix: '`nr->seq = max(src->seq)`；选组时按 seq 连续取；压实产物 `next_seq_++`。',
  why: '不用"产物 seq = 新鲜值"统一处理层合并：那会让产物排到所有未参加合并的新 run 之前，反过来让产物里的老值赢（坑 90 的反面）。两种产物语义不同，规则就不同。',
  verify: 'v2 §5.3 用例；g9 / g10 坑 90、91 回归。',
  cost: '无。',
  lesson: '任何改变"身份顺序"的操作（合并、压实、重编号）都要重新证明读路径的全序不变。',
  rel: [3, 16, 90, 91, 36],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `merge_run_set` @5954 / @6081；老代码按 §5.3 复原',
    old: `- nr->seq = src[0]->seq;          // 取了组里最老的 seq ⇒ 产物排在中间 run 之后 ⇒ 中间 run 的老值赢`,
    new: `  u32 exp = 0, out_ver = 0, maxseq = 0;
! for (RunPtr r : src) { ... maxseq = std::max(maxseq, r->seq); }
! nr->seq = maxseq;      // 保持与“未参与合并的 run”的相对新旧关系（组在全库 seq 上连续：坑 91）
  // 单元压实产物：nr->seq = next_seq_++; nr->lvl = 0;   // 坑 90`
  },
  old: [
    { t: 'seq 顺序：R1(1){k@v1} < R2(2){k@v2} < R3(3)。合并组 {R1, R3}（跨过了 R2）。', lvl: { L0: ['R3:R3 seq3:data', 'R2:R2 seq2 k@v2:data', 'R1:R1 seq1 k@v1:data'] }, kv: { 'get(k)': 'v2 ✓' } },
    { t: '产物 P 取 seq=1（最老）：P{k@v1,…} 排在 R2 之后 ⇒ R2 的 k@v2 赢。看似正确，但若组是 {R2,R3} 取 seq=2 而 R1 含更老值……反例无穷。核心错误：产物位置与内容不一致。', lvl: { L1: ['P:P seq1 {k@v1}:bad', 'R2:R2 seq2 k@v2:data'] }, kv: { '规则': '不一致 ✗' }, ok: false },
  ],
  new: [
    { t: '组必须连续：{R1,R2} 或 {R2,R3}。合并 {R1,R2}：产物 seq = max = 2，内容 {k@v2}。', lvl: { L0: ['R3:R3 seq3:data'], L1: ['P:P seq2 {k@v2}:new'] }, kv: { 'get(k)': 'v2 ✓' }, ok: true },
    { t: '单元压实产物：取新鲜 seq、进 L0（坑 90）——它只含单元内赢家，排到最前面才对。', lvl: { L0: ['Q:Q seq4（压实产物）:good', 'R3:R3 seq3:data'] }, ok: true },
  ],
});

PIT({
  id: 10, round: 'v2', cat: '存活账', found: 'v2 分层合并', tags: ['live 账', 'tombstone', '源 run'],
  sym: '分层合并之后，一条还"活着"的 tombstone（下面还有旧值要盖）被当成垃圾回收，旧值复活。',
  cause: '合并时顺手递减源 run 的存活账，墓碑被算成"死"，源 run 全死 ⇒ 退休 ⇒ 墓碑消失。',
  rule: '**源 run 随后整条删除，其账无需维护；新产物的账单独算。**改一个马上要销毁的对象的账只会制造错误决策。',
  fix: '合并期间不触碰源 run 的 `live_bytes/live_recs`；产物的账由归并时 emit 的记录直接算（R1 之后用单元级增量账）。',
  verify: 'v2 合并 + 删除回归。',
  cost: '无。',
  lesson: '先想清楚对象的生命周期，再决定它的哪些字段值得维护。',
  rel: [15, 34, 94],
  code: { kind: 'sketch', src: '按 §12 第 10 条复原',
    old: `  // 归并回调里
! if (dropped_as_old_copy) src_run->live_recs--;        // 墓碑也被减掉 ⇒ 源 run"全死"⇒ 退休 ⇒ 墓碑消失`,
    new: `  // 归并回调里：源 run 的账不动（它马上整条删除）
+ // 产物的账 = emit 出去的记录直接累计（lb += osz）
  nr->live_bytes = lb;`
  },
  old: [
    { t: '源 R{k†} 与 R\'{k@v1 更老} 合并……等等：墓碑 k† 要盖住 R\'\' 里更老的值。回调递减源账：k† 被算死。', lvl: { L1: ['R:R k† live=1→0:bad'], L2: ['R2:R2 k@v0:data'] }, kv: { 'R.live_recs': '0 ✗' } },
    { t: 'R "全死" ⇒ 退休 ⇒ 墓碑消失 ⇒ k@v0 复活。', lvl: { L2: ['R2:R2 k@v0:bad'] }, kv: { 'get(k)': 'v0 ✗' }, ok: false },
  ],
  new: [
    { t: '源账不动；产物 P 继承 k†（下面还有 R2 可能含 k ⇒ 保留，坑 94）。', lvl: { L1: ['P:P k†:new'], L2: ['R2:R2 k@v0:data'] }, kv: { 'get(k)': 'NOTFOUND ✓' }, ok: true },
  ],
});

PIT({
  id: 11, round: 'v2', cat: '格式 / 工程', found: '记录头 16 B → 10 B 改造', tags: ['REC_HDR', 'grep', '一次改全'],
  sym: '改完记录头宽度后"提交后 get 不到"。',
  cause: '`off + 16 <= used` 这类边界判断散落 4 处，改成 10 时漏了几处：页内最后一两条记录被判为"放不下"而跳过。',
  rule: '**格式常量只能有一个名字**（`REC_HDR`），所有边界判断引用它；改完用 `grep` 把魔数清零。',
  fix: '统一 `REC_HDR`；改完 `grep "16 <= used"` 与 `", 16)"` 为零。',
  verify: 'v2 全量测试；`grep` 作为改造清单的一部分。',
  cost: '无。',
  lesson: '魔数每出现一次就是一个未来的 bug；改格式先做"常量化"再改值。',
  rel: [12, 21],
  code: { kind: 'sketch', src: '按 §12 第 11 条复原',
    old: `- while (off + 16 <= used) { ... off += 16 + len; }     // 4 处之一：16 没改成 10
- ...
- std::memcpy(p + off, hdr, 16);`,
    new: `+ static constexpr u32 REC_HDR = 10;                      // key(8) + len(2)
! while (off + REC_HDR <= used) { ... off += REC_HDR + len; }
  std::memcpy(p + off, hdr, REC_HDR);
  // 改完：grep -n '16 <= used' include/  → 0 处；grep -n ', 16)' → 0 处`
  },
  old: [
    { t: '页里最后一条记录头在 off=4080，used=4090（10 B 头刚好放下）。', rows: [{ n: '页尾', items: ['r:记录 @4080 (10 B):data', 'u:used=4090:sb'] }] },
    { t: '漏改的判断 `off + 16 <= used`：4096 > 4090 ⇒ 认为放不下 ⇒ **跳过最后一条** ⇒ get 不到。', rows: [{ n: '页尾', items: ['r:记录 @4080 被跳过:bad'] }], kv: { 'get(最后一键)': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '`off + REC_HDR <= used`：4090 ≤ 4090 ⇒ 读到。grep 清零魔数。', rows: [{ n: '页尾', items: ['r:记录 @4080 ✓:good'] }], kv: { 'get(最后一键)': '找到 ✓' }, ok: true },
  ],
});

PIT({
  id: 12, round: 'v2', cat: '设备约束', found: 'RamDev::valid 断言', tags: ['部分页写', '4 B 对齐', 'RC::IO'],
  sym: '整次提交返回 `RC::IO`，设备模型报非法写。',
  cause: '部分页写（只写页内一段）的偏移 / 长度没有 4 B 对齐；`RamDev::valid` 要求 `off%4==0 && len%4==0`（模拟真实 NOR / 块设备的写粒度）。',
  rule: '**写粒度是设备契约**：所有部分写路径把 `off/len` 向下 / 向上对齐到 4 B。',
  fix: '写前 `off &= ~3; len = (len + 3) & ~3;`，并保证页内布局本身 4 B 对齐（记录头 10 B + 2 B 对齐）。',
  verify: 'v2 全量；`RamDev::valid` 常驻。',
  cost: '每次部分写最多多 3 字节。',
  lesson: '设备模型里的断言是最便宜的契约检查，要早加、常开。',
  rel: [6, 11],
  code: { kind: 'sketch', src: '按 §12 第 12 条复原',
    old: `- dev_->write_part(pid, off, len, buf);          // off=4090, len=6 ⇒ RamDev::valid 拒绝 ⇒ RC::IO`,
    new: `+ u32 a = off & ~3u, b = (off + len + 3) & ~3u;
! dev_->write_part(pid, a, b - a, page + a);     // 4 B 对齐的区间`
  },
  old: [
    { t: '部分写 off=4090, len=6。', rows: [{ n: '写请求', items: ['w:off 4090 len 6:bad'] }], kv: { 'valid': 'false ✗', '提交': 'RC::IO' }, ok: false },
  ],
  new: [
    { t: '对齐到 [4088, 4096)。', rows: [{ n: '写请求', items: ['w:off 4088 len 8:good'] }], kv: { 'valid': 'true ✓' }, ok: true },
  ],
});

PIT({
  id: 13, round: 'v2', cat: '读路径 / 版本', found: 'v2 游标 + 快照', tags: ['cursor', 'run.ver', '视图过滤'],
  sym: '游标（scan）在快照视图下读到了比视图更新的记录。',
  cause: '记录版本已经不在记录头里（10 B 头只有 key+len）；版本挂在 **run** 上（`run->ver`）。游标按段遍历时忘了按 run 的 ver 过滤。',
  rule: '**可见性过滤在 run 粒度**：`run->ver > view` 的整条 run 对该视图不可见。点查、游标、归并都走同一条过滤。',
  fix: '游标迭代器跳过 `ver > view` 的段。',
  verify: 'v2 快照 scan 回归；后续 g7 并发快照段。',
  cost: '无。',
  lesson: '把元数据从记录头上移到 run 上之后，每个读路径都要重新接上这个元数据。',
  rel: [16, 69],
  code: { kind: 'sketch', src: '按 §12 第 13 条复原',
    old: `  for (auto& sg : c->segs) {
!     // 没有 ver 过滤：快照视图也看见新 run
      push_cursor(sg);
  }`,
    new: `  for (auto& sg : c->segs) {
!     if (run_of(sg)->ver > view) continue;     // 视图过滤在 run 粒度
      push_cursor(sg);
  }`
  },
  old: [
    { t: '快照 view=5。段：R1(ver 3){a} R2(ver 8){b}。', rows: [{ n: 'segs', items: ['R1:R1 ver3 {a}:data', 'R2:R2 ver8 {b}:data'] }] },
    { t: '游标不看 ver ⇒ 输出 a, b。b 对 view=5 不该可见。', rows: [{ n: 'scan 输出', items: ['a:a:data', 'b:b ✗:bad'] }], ok: false },
  ],
  new: [
    { t: '`ver 8 > view 5` ⇒ 跳过 R2 ⇒ 输出 a。', rows: [{ n: 'scan 输出', items: ['a:a:good'] }], ok: true },
  ],
});

PIT({
  id: 14, round: 'v2', cat: '异步写 / 页复用', found: 'v2 异步组提交', tags: ['commit(false)', 'async', 'pending_free_', 'promote'],
  sym: '异步模式（`commit(false)`）下崩溃后旧态也坏了：旧清单引用的页里出现新内容。',
  cause: '`commit(false)` 已经把页交给后台线程写，但清单还在旧状态；若此时 `free_page` 让某页被重新分配，后台线程会把新内容写到"还挂在旧清单上"的页。',
  rule: '**异步 flush 只做 flush**；回收 / 合并一律留到提交点（`pending_free_` 之后才 promote）。',
  fix: '异步路径不退休 run、不 promote；所有释放等 `commit()`。',
  verify: 'v2 异步 + 掉电；后续 g6 异步种子。',
  cost: '异步模式下空间回收延迟到下一个同步提交。',
  lesson: '后台线程持有的"在飞页"是一种隐式引用，释放逻辑必须把它算进所有者集合。',
  rel: [5, 25, 99],
  code: { kind: 'sketch', src: '按 §12 第 14 条复原',
    old: `  RC commit(bool durable) {
      flush_mem();                      // async：页交给后台线程
!     retire_dead_runs(); promote_pending();   // 清单还是旧的，页却回 free_ ⇒ 后台线程写进"旧清单仍引用"的页
      if (durable) write_sb();
  }`,
    new: `  RC commit(bool durable) {
      flush_mem();
!     if (!durable) return RC::OK;      // 异步 flush 只做 flush：不退休、不 promote
      retire_dead_runs(); commit_delta(); write_sb(); promote_pending();
  }`
  },
  old: [
    { t: '`commit(false)`：后台线程排队写 run N 到页 50~52；同时退休 R（页 20）并 promote。', rows: [{ n: '盘上清单（旧）', items: ['R:R 页20:data'] }, { n: '后台队列', items: ['N:N → 50-52:new'] }, { n: 'free_', items: ['f:20:free'] }] },
    { t: '下一批 flush 复用页 20 给 run M，交给后台线程。清单仍旧。', rows: [{ n: '盘上清单（旧）', items: ['R:R 页20（即将被 M 覆盖）:bad'] }, { n: '后台队列', items: ['N:N → 50-52:new', 'M:M → 20:new'] }] },
    { t: '崩溃：盘上清单引用 R@20，页 20 已是 M ⇒ 旧态坏。', kv: { 'REPLAY R': 'CRC ✗' }, ok: false },
  ],
  new: [
    { t: '`commit(false)` 只 flush；R 不退休、页 20 不释放。', rows: [{ n: '盘上清单（旧）', items: ['R:R 页20:data'] }, { n: '后台队列', items: ['N:N → 50-52:new'] }], ok: true },
    { t: '`commit()`（同步）：等在飞 I/O → 退休 R → 清单 + SB → promote。崩溃任意时刻都有一致的旧态。', rows: [{ n: '盘上清单（新）', items: ['N:N 50-52:good'] }, { n: 'free_', items: ['f:20:free'] }], ok: true },
  ],
});

PIT({
  id: 15, round: 'v2', cat: '存活账', found: 'v2 账漂移', tags: ['recount', 'live/dead', '单一算术'],
  sym: '"live 漂移"：单元该分裂不分裂、空间虚高、压实选错受害者；`verify` 一开始查不出来（两边用同一份错账）。',
  cause: '`live/dead/pages` 的增量补丁散在五六个地方（挂段、摘段、升层、压缩、分裂），漏一处就漂。',
  rule: '**记账只能有一条算术**：`recount(cell)` 从段集合直接算出 live/dead/pages；结构性改写之后统一重算。增量路径只能是对这条算术的**可证明等价**的优化（R1 的单元级增量账 + FCS_TRACE 对账）。',
  fix: '引入 `recount(cell)` 为唯一入口；`verify` 用独立口径（逐键归并）核对。',
  verify: 'v2 起 `verify` 每轮核对；第 11/14 轮增量账用 FCS_TRACE 与 recount 对账 0 mismatch。',
  cost: '结构性改写后一次 O(段 × 页) 重算（后续用增量账优化并对账）。',
  lesson: '派生量（账）要有唯一的真值函数；性能优化只能在真值函数旁边加缓存，不能替换它。',
  rel: [27, 34, 35, 'r14-3'],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `recount()` @5610；老代码按 §12 第 15 条复原',
    old: `- c->live += n;            // 挂段处
- c->live -= m;            // 摘段处
- c->live = c->live / 2;   // 分裂处（估的）
- ...                      // 五六处各自为政，漏一处就漂`,
    new: `  // 精确重算单元 live/dead（读页，但只读该单元的页）
  // 单元记账的唯一算术入口：live/dead/pages 都由"段集合"直接算出来。
! void recount(Cell* c) {
      ++recount_calls_;
      ... k 路归并该单元全部段：赢家字节 → live，其余 → dead；pages 由段的 [p0,p1) 求和 ...
  }
  // 结构性改写（挂段 / 摘段 / 升层 / 压缩 / 分裂）之后统一 recount；R1 的增量账用 FCS_TRACE 与它对账`
  },
  old: [
    { t: '单元 live 真值 100。挂段 +30 ✓、摘段 −20 ✓、分裂估 /2（真值不是一半）……', kv: { 'live（账）': 100, 'live（真）': 100 } },
    { t: '几轮之后账 140、真值 100：该分裂不分裂、压实选错受害者。`verify` 若用同一份账则查不出。', kv: { 'live（账）': '140 ✗', 'live（真）': 100 }, ok: false },
  ],
  new: [
    { t: '结构性改写后 `recount(cell)`：账 = 真值。`verify` 用逐键归并独立口径核对。', kv: { 'live（账）': '100 ✓', 'live（真）': 100 }, ok: true },
  ],
});

PIT({
  id: 16, round: 'v2', cat: '读路径 / 胜者规则', found: 'v2 合并产物读到旧值', tags: ['winner', '(seq, rid)', 'ver', '段顺序'],
  sym: '在"低 seq + 高版本"的合并产物上读到旧值。',
  cause: '读路径按"段在 cell 里的排列先后"取第一个命中。段的排列只表示归属，不是时间序。',
  rule: '**胜者规则只能有一条**：可见版本最大者胜，同版本 seq 新者胜。读路径与归并统一用 `(seq, rid)` 全序。',
  fix: '点查 / 游标 / 归并共用一个比较器。',
  verify: 'v2 用例；后续所有读一致性测试。',
  cost: '无。',
  lesson: '"顺序"有多种（存储顺序、时间顺序、键顺序），每处比较都要写清用哪一种。',
  rel: [3, 9, 13],
  code: { kind: 'sketch', src: '按 §12 第 16 条复原',
    old: `- for (auto& sg : c->segs) if (find_in(sg, k, &v)) return v;   // 按段排列先后：第一个命中就返回`,
    new: `! // 候选按 (ver 大者, seq_key 大者) 取胜 —— 与归并同一比较器
  best = none; for (auto& sg : c->segs) if (find_in(sg, k, &cand) && better(cand, best)) best = cand;
  return best;`
  },
  old: [
    { t: '段排列：S_a(seq 9, ver 2){k=old} 在前，S_b(seq 4, ver 5){k=new} 在后（合并产物 seq 低但版本高）。', rows: [{ n: 'segs 排列', items: ['a:S_a seq9 ver2 k=old:data', 'b:S_b seq4 ver5 k=new:data'] }] },
    { t: '按排列先后 ⇒ 返回 old。', kv: { 'get(k)': 'old ✗' }, ok: false },
  ],
  new: [
    { t: '统一比较器：ver 5 > ver 2 ⇒ new 胜。', kv: { 'get(k)': 'new ✓' }, ok: true },
  ],
});

PIT({
  id: 17, round: 'v2', cat: '空间 / 收缩', found: 'v2 shrink 不生效', tags: ['shrink', 'checkpoint', '清单链', '高水位'],
  sym: '数据都搬到低处了，`shrink()` 之后高水位还是下不来。',
  cause: '清单链的页是当年随手分配的，飘在高处；活区顶由它们决定。',
  rule: '**高水位由所有活页决定，包括元数据页。**要收缩先把元数据也搬下来。',
  fix: '先 `checkpoint()` 重写清单（新链落在低处），再 `shrink()`。',
  verify: 'v2 收缩测试；`compact_file` 流程固化为 归一 → checkpoint → shrink。',
  cost: 'checkpoint 一次全量清单写。',
  lesson: '收缩类操作要列出"所有占据高地址的对象"，而不只是数据。',
  rel: [18, 28, 'r14-5'],
  code: { kind: 'sketch', src: '按 §12 第 17 条复原',
    old: `- relocate_runs_down(); shrink();      // 清单页仍飘在高处 ⇒ live_top 不降`,
    new: `  relocate_runs_down();
! checkpoint();                         // 重写清单链到低处
  shrink();`
  },
  old: [
    { t: '数据 run 搬到低处；清单页 M1~M3 还在高处。', rows: [{ n: '页', items: ['d:数据 0-100:data', 'h:洞:hole', 'm:清单 900-902:man'] }], kv: { 'live_top': 903 } },
    { t: '`shrink()`：活区顶 903 ⇒ 不收缩。', kv: { 'tail_': '903 ✗' }, ok: false },
  ],
  new: [
    { t: '`checkpoint()`：新清单链写到低处 101~103，旧链释放。', rows: [{ n: '页', items: ['d:数据 0-100:data', 'm:清单 101-103:man', 'h:洞:hole'] }], kv: { 'live_top': 104 } },
    { t: '`shrink()` ⇒ tail_=104。', kv: { 'tail_': '104 ✓' }, ok: true },
  ],
});

PIT({
  id: 18, round: 'v2', cat: '空间 / 分配器', found: 'v2 搬家 / 收缩', tags: ['free_', 'map<起始页, 页数>', '最低地址', '邻居合并'],
  sym: '搬家 / 收缩总是挑不到"最低的洞"，高水位下不来；邻居洞也合不起来。',
  cause: '空闲表是 `multimap<长度, 起始页>`（按大小最优适配）；而搬家 / 收缩要的是**最低地址**，按长度索引找不到。',
  rule: '**空闲表按地址索引**：`map<起始页, 页数>` —— 低地址优先分配（first-fit by address），邻居合并只需看一眼后继。同一个数据结构，两个用途。',
  fix: '`std::map<u32, u32> free_`；`alloc_pages` 从低地址遍历；`coalesce` 看前驱 / 后继。',
  verify: 'v2 收缩测试；第 14 轮 layout 探针继续沿用。',
  cost: '分配是 O(洞数) 遍历（洞数小；第 14 轮加了簇跳过）。',
  lesson: '数据结构的索引键要服务于最频繁且最关键的查询——这里是"最低可用地址"，不是"最合适的大小"。',
  rel: [17, 70, 71, 'r14-5'],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `free_` 声明；老代码按 §12 第 18 条复原',
    old: `- std::multimap<u32, u32> free_;   // 长度 → 起始页（最优适配）
- auto it = free_.lower_bound(n);   // 找"够大"的，地址随机 ⇒ 搬家搬到高处、邻居合并要全表扫`,
    new: `! std::map<u32, u32> free_;      // 起始页 → 页数（按地址有序：低地址优先分配）
  // alloc_pages：从 begin() 遍历第一个 ≥ n 的洞（最低地址）
  // coalesce：erase 后看 prev / next 是否相邻即可`
  },
  old: [
    { t: '洞：{长 5 @页 800, 长 6 @页 10}。搬 5 页：最优适配选长 5 ⇒ 页 800（高处）。', rows: [{ n: '页', items: ['a:洞 6 @10:hole', 'd:数据:data', 'b:洞 5 @800:hole'] }], kv: { '选中': '@800 ✗' }, ok: false },
  ],
  new: [
    { t: '按地址遍历：第一个 ≥5 的洞是 @10 ⇒ 搬到低处；邻居合并看后继即可。', rows: [{ n: '页', items: ['a:→ @10:good', 'd:数据:data', 'b:洞 5 @800:hole'] }], kv: { '选中': '@10 ✓' }, ok: true },
  ],
});
