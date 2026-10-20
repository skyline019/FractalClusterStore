// 第 2 批（b）：坑 19 ~ 36（v2 异步写 / 性能 / 回退 / 存活账）。无历史头：代码块按 §12 复原（sketch），
// 能从当前 include/fcs.h 原样引用的新代码标 half。所有数字来自 DESIGN3 §12（当年实测），非本轮复测。

PIT({
  id: 19, round: 'v2', cat: '异步写 / 落盘粒度', found: 'v2 异步组提交基准', tags: ['async', '停顿分布', '按字节落段', '8192 条'],
  sym: '以为异步写能提吞吐，实测只持平（4.3 → 4.1 µs/op）；而"每 8192 条落一段"的版本反而**慢 2.3×**（4.6 → 10.7 µs/op）。',
  cause: '页写本来就是批次一次顺序写，前台 CPU 才是瓶颈——异步换到的是工作区 8.4 MB → 2.8 MB 与更平缓的停顿，不是吞吐。而按**提交条数**落段会制造 24 条键域全重叠的小 run：每次读都要查全部，归并也要全部参加。',
  rule: '**落盘粒度按字节（工作区水位）不按提交数**；异步写的指标是停顿分布 / 内存，不是峰值吞吐。',
  fix: '`flush` 触发条件改为 `mem_bytes >= FCS_MEM_FLUSH`；异步 worker 只搬 I/O。',
  why: '不追求"异步提吞吐"：CPU 侧（编码 + 归并 + 账）不会因为 I/O 异步而变少；把目标写对，才不会为了一个达不到的数字扭曲设计。',
  verify: 'v2 bench：异步 4.1 µs/op 持平、工作区 2.8 MB；按字节落段恢复 4.6 µs/op 量级。',
  cost: '异步路径引入 `wait_io()` 等同步点（坑 25）。',
  lesson: '先量瓶颈再决定把什么挪后台；"小而多的 run"在 LSM 里是毒药，落段粒度要服务读路径。',
  rel: [14, 25, 48],
  code: { kind: 'sketch', src: '按 §12 第 19 条复原',
    old: `- if (mem_recs_ >= 8192) flush_mem();      // 按条数落段：24 条键域全重叠的小 run
- // 读：每条 run 都要查；合并：都参加 ⇒ 4.6 → 10.7 µs/op`,
    new: `! if (mem_bytes_ >= FCS_MEM_FLUSH) flush_mem();   // 按字节：段大、数量少
  // 异步 worker 只做页写；指标看停顿分布与工作区（8.4 → 2.8 MB），吞吐持平即可`
  },
  old: [
    { t: '按条数落段：同一键域上堆出 24 条小 run。', lvl: { L0: ['r1:run:data', 'r2:run:data', 'r3:run:data', 'r4:…×24:data'] }, kv: { '每次 get 查 run 数': 24 } },
    { t: '时间线：前台每 8192 条就停下来落段 + 归并——停顿密集，吞吐 10.7 µs/op。', lanes: [{ n: '前台', ev: [{ a: 0, b: 12, l: '写' }, { a: 12, b: 22, l: '落段', k: 'wait' }, { a: 22, b: 34, l: '写' }, { a: 34, b: 44, l: '落段', k: 'wait' }, { a: 44, b: 56, l: '写' }, { a: 56, b: 66, l: '落段', k: 'wait' }, { a: 66, b: 78, l: '写' }, { a: 78, b: 88, l: '落段', k: 'wait' }] }], t0: 't=0', t1: '10.7 µs/op', kv: { 'µs/op': '10.7 ✗' }, ok: false },
  ],
  new: [
    { t: '按字节落段：run 少而大。', lvl: { L0: ['r1:run（大）:good', 'r2:run（大）:good'] }, kv: { '每次 get 查 run 数': 2 } },
    { t: '异步 worker 搬 I/O，前台停顿平缓；吞吐 4.1 µs/op（持平），工作区 2.8 MB。', lanes: [{ n: '前台', ev: [{ a: 0, b: 45, l: '写' }, { a: 45, b: 50, l: '落段', k: 'wait' }, { a: 50, b: 95, l: '写' }] }, { n: 'worker', ev: [{ a: 46, b: 70, l: '页写 I/O', k: 'io' }] }], t0: 't=0', t1: '4.1 µs/op', kv: { 'µs/op': '4.1 ✓', '工作区': '8.4 → 2.8 MB' }, ok: true },
  ],
});

PIT({
  id: 20, round: 'v2', cat: '结构 / 切分', found: 'v2 性能剖析（30k 全扫）', tags: ['split', 'retighten', '页区间', 'O(n²)'],
  sym: '一切都"正确"，只是慢：30k 条全扫 698 ns/条，一次 50k 提交 195 ms，`verify` 74 ms。',
  cause: '`split()` 只改键域不动页区间（零数据搬移，这是对的），但页区间**不收窄**的话每个子单元都要从 run 的首页扫到尾页——单元数 × 页数 ⇒ O(n²)。',
  rule: '**段的页区间必须是"键域真正覆盖的页"的紧界**；切分之后立刻按 fence 收紧（`retighten`）。',
  fix: '`retighten(Cell*)`：对每段用 run 的 fence 二分出 `[p0, p1)`，并重算 `c->pages`。',
  why: '不改成"切分时搬数据"：搬数据让切分变成写操作，违反"切分零 I/O"的初衷；收紧页区间是纯内存计算。',
  verify: '30k 全扫 698 → 68 ns/条（10×）；50k 提交 195 → 22.6 ms（8.6×）；`verify` 74 → 6.5 ms。',
  cost: '每次切分对每段做一次 fence 二分。',
  lesson: '不违反任何不变量的 bug 最难发现——要用剖析器和 O() 推演守住复杂度。',
  rel: [1, 29, 92],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `retighten(Cell*)`（约 3296 行）；老代码按 §12 第 20 条复原',
    old: `  void split(Cell* c, u64 mid) {
      ... 子单元各自拿到键域 [lo,mid) / [mid,hi) ...
!     // 段的 p0/p1 原样复制：每个子单元仍然从 run 首页扫到尾页 ⇒ 单元数 × 页数
  }`,
    new: `  // 单元被切分后，段的键域变窄 ⇒ 页区间也必须跟着收紧。
  // 不收紧的话每个单元都要从 run 的首页开始扫（切分 N 次就是 O(N·页数)）。
  void retighten(Cell* c) {
      c->pages = 0;
      for (auto& sg : c->segs) {
          RunPtr r = runs_[sg.second.rid];
          if (!r) continue;
          u32 p0, p1;
!         page_range(r, sg.second.klo, sg.second.khi, p0, p1);   // 按 fence 二分出紧界
          sg.second.p0 = p0;
          sg.second.p1 = p1;
          c->pages += p1 - p0;
      }
  }`
  },
  old: [
    { t: 'run R 有 8 页，键单调。单元 C=[0,800) 切成 8 个子单元。', rows: [{ n: 'run R', items: ['p0:p0 0-99:data', 'p1:p1:data', 'p2:p2:data', 'p3:p3:data', 'p4:p4:data', 'p5:p5:data', 'p6:p6:data', 'p7:p7 700-799:data'] }] },
    { t: '每个子单元的段仍是 [p0, p8)：查键 750 要从 p0 扫起。8 单元 × 8 页 = 64 次页读。', rows: [{ n: '单元 [700,800) 扫描', items: ['p0:p0:bad', 'p1:p1:bad', 'p2:p2:bad', 'p3:p3:bad', 'p4:p4:bad', 'p5:p5:bad', 'p6:p6:bad', 'p7:p7 命中:data'] }], kv: { '页读 / 查询': 8, '30k 全扫': '698 ns/条 ✗', 'verify': '74 ms' }, ok: false },
  ],
  new: [
    { t: '`retighten`：子单元 [700,800) 的段收紧为 [p7, p8)。', rows: [{ n: 'run R', items: ['p0:p0:dim', 'p1:p1:dim', 'p2:p2:dim', 'p3:p3:dim', 'p4:p4:dim', 'p5:p5:dim', 'p6:p6:dim', 'p7:p7 ← 段:good'] }] },
    { t: '查键 750 直达 p7；8 单元 × 1 页。', kv: { '页读 / 查询': '1 ✓', '30k 全扫': '68 ns/条 ✓', '50k 提交': '195 → 22.6 ms', 'verify': '74 → 6.5 ms' }, ok: true },
  ],
});

PIT({
  id: 21, round: 'v2', cat: '常数项 / CRC', found: 'v2 verify / mount 剖析', tags: ['crc32c', '_mm_crc32_u64', '7.5 GB/s'],
  sym: '`verify` / `mount` / 每次提交的常数项大，剖析发现 CRC 占大头。',
  cause: '查表 CRC 374 MB/s，一页 4 KB 要 11 µs；每页一次校验。',
  rule: '**页校验走硬件 `crc32` 指令**（x86 SSE4.2），差 20 倍：7.5 GB/s，一页 0.6 µs。',
  fix: '`common.h` 的 `crc32c_hw()` 用 `_mm_crc32_u64` / `_mm_crc32_u8`；页 CRC 覆盖 `p+12 .. used`。',
  why: '不是"减少校验次数"：每页一次是最低频率；把常数项做对比减少校验安全得多。',
  verify: '实测 374 MB/s → 7.5 GB/s；每页 11 µs → 0.6 µs。',
  cost: '需要 SSE4.2（本项目目标平台具备；无指令时可回退查表）。',
  lesson: '20 倍的常数项差距足以改变"值不值得校验"的决策——先把常数项做对。',
  rel: [11, 26],
  code: { kind: 'half', src: '新代码 = `include/common.h:140 crc32c_hw` 与 `include/fcs.h` 页校验调用（约 296 行）；老代码按 §12 第 21 条复原',
    old: `- static u32 crc32c_table(const u8* p, size_t n) {       // 查表：374 MB/s，一页 11 µs
-     u32 c = ~0u; for (size_t i = 0; i < n; ++i) c = T[(c ^ p[i]) & 0xff] ^ (c >> 8); return ~c;
- }`,
    new: `  // common.h
  __attribute__((target("sse4.2")))
  inline u32 crc32c_hw(const void* data, size_t n, u32 crc) {   // 7.5 GB/s，一页 0.6 µs
      const unsigned char* p = (const unsigned char*)data;
      unsigned long long c = ~(unsigned long long)crc;
      while (n >= 8) {
          unsigned long long v;
          std::memcpy(&v, p, 8);
!         c = _mm_crc32_u64(c, v);
          p += 8; n -= 8;
      }
      while (n) { c = _mm_crc32_u8((u32)c, *p++); --n; }
      return ~(u32)c;
  }
  // fcs.h 页头：crc 覆盖 [12, used)
  wr32(p + 8, crc32c(p + 12, (size_t)(used - 12)));`
  },
  old: [
    { t: '每页一次查表 CRC：11 µs/页。10 000 页的 verify 光 CRC 就 110 ms。', kv: { 'MB/s': 374, 'µs/页': '11 ✗' }, ok: false },
  ],
  new: [
    { t: '硬件 crc32：0.6 µs/页；同样 10 000 页 6 ms。', kv: { 'GB/s': 7.5, 'µs/页': '0.6 ✓' }, ok: true },
  ],
});

PIT({
  id: 22, round: 'v2', cat: '设备 / 容量', found: 'v2 shrink 之后写满', tags: ['truncate', 'max_pages', 'cap_', 'dev_pages_'],
  sym: '`shrink()` 之后设备再也长不回去：某天写到 `NOSPACE`，之后**永远**写不进——连删都删不掉（删除也要落页）。',
  cause: '`truncate()` 只能减不能增：逻辑容量和物理长度是同一个数，`shrink` 把两者一起砍了。',
  rule: '**逻辑容量（`cap_`）取设备上限 `max_pages`，物理长度（`dev_pages_`）单独记**；分配越过物理长度时按需 `truncate()` 增长。',
  fix: '`BlockDev::max_pages`；`cap_ = max_pages ? max_pages : capacity()`；`dev_pages_ = capacity()`；`ensure_grow(tail)`。',
  why: '不取消 shrink：收缩是文件设备的核心价值；让它只影响物理长度即可。',
  verify: 'v2 收缩后继续写满测试；`FileDev(path, npages, psize, create)` 持续沿用。',
  cost: '多一个字段 + 一次增长判断。',
  lesson: '"容量"是两个概念：能用多大、现在多长。合并成一个数就会出现单向阀。',
  rel: [17, 23, 28],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` 约 929 行；老代码按 §12 第 22 条复原',
    old: `- cap_ = dev->capacity();                 // shrink 后 capacity 变小 ⇒ cap_ 永久封顶
  RC shrink() { dev_->truncate(live_top); cap_ = live_top; }`,
    new: `! cap_ = dev->max_pages ? dev->max_pages : dev->capacity();   // 逻辑容量 = 上限
! dev_pages_ = dev->capacity();                               // 物理长度单独记
  RC shrink() { dev_->truncate(live_top); dev_pages_ = live_top; }    // 不动 cap_
  // 分配：if (pid + n > dev_pages_) dev_->truncate(grow_to) /* 按需增长 */`
  },
  old: [
    { t: '设备 1000 页，`shrink()` 到 200 页：`cap_ = 200`。', rows: [{ n: '容量', items: ['c:cap_=200:bad'] }], kv: { 'cap_': '1000 → 200' } },
    { t: '写到 200 页 ⇒ NOSPACE；删除也要落页 ⇒ 也 NOSPACE ⇒ 永久锁死。', kv: { 'put': 'NOSPACE ✗', 'del': 'NOSPACE ✗' }, ok: false },
  ],
  new: [
    { t: '`shrink()` 只减 `dev_pages_`；`cap_` 仍 1000。', rows: [{ n: '容量', items: ['c:cap_=1000:good', 'd:dev_pages_=200:free'] }] },
    { t: '继续写：越过 200 时 `truncate(grow)` 增长。', kv: { 'put': 'OK ✓', 'dev_pages_': '200 → 300' }, ok: true },
  ],
});

PIT({
  id: 23, round: 'v2', cat: '满库 / 预留', found: 'v2 满库测试', tags: ['NOSPACE', '7/8', '预留区', 'probe_prev'],
  sym: '"满了 → 要删 → 删也要空间 → 还是满"的死锁；只给用户 7/8 容量不够。',
  cause: '一批全是覆盖 / 删除的写并不增长存活集，却被当成普通写拒绝。',
  rule: '**满库必须删得掉**：当"新字节 ≤ 被替换字节"时允许动用预留区。',
  fix: '`flush_mem` 里一次 `probe_prev` 求和得到被替换字节；满足判据则放行到预留区。',
  why: '不用"预留给删除专用的固定页"：固定预留会被一批大 value 的覆盖耗尽，判据比配额稳。',
  verify: 'v2 满库 + 全删 → 空间回到低位。',
  cost: '满库附近每条记录一次 `probe_prev`。',
  lesson: '资源预留的放行条件应当是"这笔操作最终让资源变多还是变少"，不是操作类型。',
  rel: [22, 24, 44],
  code: { kind: 'sketch', src: '按 §12 第 23 条复原',
    old: `- if (used_ + need > cap_ * 7 / 8) return RC::NOSPACE;     // 删除 / 覆盖也被挡`,
    new: `+ u64 replaced = 0; for (auto& r : mem_) replaced += probe_prev(r.key).bytes;   // 被替换字节
! bool shrinking = need <= replaced;
! if (!shrinking && used_ + need > cap_ * 7 / 8) return RC::NOSPACE;   // 收缩型批次可用预留区`
  },
  old: [
    { t: '用户区满（7/8）。用户要删 1000 条以腾空间。', rows: [{ n: '设备', items: ['u:用户区 7/8 满:data', 'r:预留 1/8:res'] }] },
    { t: '墓碑也要落页 ⇒ 用户区没空间 ⇒ NOSPACE ⇒ 死锁。', kv: { 'del ×1000': 'NOSPACE ✗' }, ok: false },
  ],
  new: [
    { t: '`probe_prev` 求和：被替换 1000 条 × 平均 100 B = 100 KB ≥ 新字节 10 KB ⇒ 放行到预留区。', rows: [{ n: '设备', items: ['u:用户区 7/8 满:data', 'r:预留区 ← 墓碑段:new'] }], kv: { 'del ×1000': 'OK ✓' } },
    { t: '随后压实回收 100 KB，预留区又空出来。', rows: [{ n: '设备', items: ['u:用户区 ↓:data', 'r:预留 1/8:res'] }], ok: true },
  ],
});

PIT({
  id: 24, round: 'v2', cat: '维护 / 失败隔离', found: 'v2 满库 + 合并', tags: ['merge_run_set', 'NOSPACE', '机会性维护'],
  sym: '用户提交返回"库满了"，实际上是**内存里的垃圾没地方腾**：`merge_run_set` 一次 NOSPACE 把整个用户提交带崩。',
  cause: '维护（压实 / 合并 / 清单页）与用户写在同一个提交里，维护的失败没有隔离。',
  rule: '**维护是机会性的**：拿不到空间就跳过本轮，用户提交只因用户数据写不下而失败。',
  fix: '`merge_run_set` / `compact_cell` 返回 NOSPACE 时在 `maintain()` 里吞掉并记计数，不向 `commit` 传播。',
  why: '不是"先做维护腾空间再写"：维护本身要空间，先维护会更快把库撑爆。',
  verify: 'v2 满库回归；后续 `maint_skipped_` 计数器。',
  cost: '满库附近维护停摆，空间放大暂时上升（第 13 轮的分级预算回退进一步细化）。',
  lesson: '把"可以失败"的工作与"不能失败"的工作放在不同的错误域里。',
  rel: [23, 44, 106],
  code: { kind: 'sketch', src: '按 §12 第 24 条复原',
    old: `  RC commit_locked() {
      RC rc = flush_mem(); if (rc) return rc;
!     rc = merge_run_set(grp); if (rc) return rc;      // 维护 NOSPACE ⇒ 用户提交失败`,
    new: `  RC commit_locked() {
      RC rc = flush_mem(); if (rc) return rc;          // 只有用户数据写不下才失败
!     if (merge_run_set(grp) == RC::NOSPACE) { ++maint_skipped_; /* 跳过本轮 */ }`
  },
  old: [
    { t: '用户写 1 页成功；本轮顺带合并需要 40 页临时空间，只有 20 页。', rows: [{ n: '空间', items: ['u:用户 1 页 ✓:new', 'm:合并需 40 页:bad'] }] },
    { t: '合并 NOSPACE ⇒ 整个 commit 返回 NOSPACE ⇒ 用户看到"库满"。', kv: { 'commit': 'NOSPACE ✗' }, ok: false },
  ],
  new: [
    { t: '合并拿不到空间 ⇒ 跳过本轮、计数；用户提交成功。', rows: [{ n: '空间', items: ['u:用户 1 页 ✓:good', 'm:合并跳过:dim'] }], kv: { 'commit': 'OK ✓', 'maint_skipped_': '+1' }, ok: true },
  ],
});

PIT({
  id: 25, round: 'v2', cat: '异步 / 状态重建', found: 'v2 rollback 随机坏清单（追了一整轮）', tags: ['wait_io', 'rollback', 'reload', '在飞页'],
  sym: '**随机坏清单项**（`npages` 变成天文数字）；复现概率低、位置随机。',
  cause: '`rollback()` / `reload()` 按盘重建内存时**没有 `wait_io()`**：worker 手里还握着旧分支的页，写下去正好落进刚回收重分配的位置——用旧页覆盖新页。',
  rule: '**任何"按盘重建内存"之前，先把在飞的 I/O 等干净。**',
  fix: '`rollback()` / `reload()` 入口 `wait_io()`；后续所有重建路径同此。',
  why: '不靠"给在飞页打标记跳过"：在飞集合由 worker 持有，主线程看不全；等干净是唯一可证明正确的办法。',
  verify: 'v2 rollback 压力（随机坏清单消失）；后续 g6 / g12 异步回退段。',
  cost: '回退时一次 I/O 排空等待。',
  lesson: '异步系统里"状态重建"必须先建立一个所有线程都同意的静止点。',
  rel: [14, 26, 29, 33],
  code: { kind: 'sketch', src: '按 §12 第 25 条复原',
    old: `  RC rollback() {
!     // 没有 wait_io()：worker 仍在写旧分支的页
      free_.clear(); cells_.clear(); replay_manifest(); ...
  }`,
    new: `  RC rollback() {
!     wait_io();                  // 先让在飞 I/O 落地（它们写的是旧分支的页；重建后那些页可能已被重分配）
      free_.clear(); cells_.clear(); replay_manifest(); ...
  }`
  },
  old: [
    { t: 'worker 队列里还有旧分支 run X → 页 70~72 的写。主线程 `rollback()` 重建：70~72 按盘属于空闲。', rows: [{ n: 'worker', items: ['x:X → 70-72（在飞）:pend'] }, { n: '重建后 free_', items: ['f:70-72:free'] }] },
    { t: '重建后新分支把清单页分配到 71。worker 随后把 X 写进 70~72 ⇒ **旧页覆盖新清单页**。', rows: [{ n: '页 71', items: ['m:新清单页:man', 'x:← X 覆盖:bad'] }], kv: { '清单项 npages': '0x7f3a9c12 ✗' }, log: ['!apply_man: npages=2134219794 (bogus)'], ok: false },
  ],
  new: [
    { t: '`rollback()` 入口 `wait_io()`：X 先落地到 70~72。', rows: [{ n: 'worker', items: ['x:X 70-72 已落地:data'] }] },
    { t: '然后重建：70~72 按盘空闲 ⇒ 后续分配给清单页，没有任何在飞写会再碰它。', rows: [{ n: '页 71', items: ['m:新清单页 ✓:good'] }], ok: true },
  ],
});

PIT({
  id: 26, round: 'v2', cat: '清单 / 鲁棒性', found: 'v2 坏清单项', tags: ['apply_man', '越界', '自证清白', 'mount 不失败'],
  sym: '挂载时 `apply_man` 照着一条坏记录的 `npages` 分配 ⇒ 申请 9 GB 内存 / 崩溃。',
  cause: '清单项被当成可信数据直接使用。',
  rule: '**清单项必须自证清白**：页号 / 页数 / 单元键域都要在界内，越界条目一律丢弃 + 计数；挂载永远不会因为一条坏记录失败。',
  fix: '`apply_man` 前置范围检查（`page0 + npages <= cap_` 等），坏项计入 `bad_man_`。',
  why: '不用"CRC 过了就可信"：坑 25 / 29 证明坏项可以带着正确 CRC 出现（写错位置的完整页）。',
  verify: 'v2 坏清单注入；后续 `verify` 报告 `bad_man_`。',
  cost: '每项几次比较。',
  lesson: '持久化输入都是不可信输入。',
  rel: [25, 29, 80],
  code: { kind: 'sketch', src: '按 §12 第 26 条复原',
    old: `- case M_RUN_ADD: runs_.emplace(m.seq, make_run(m.page0, m.npages));   // npages 来自盘，未校验`,
    new: `  case M_RUN_ADD:
!     if ((u64)m.page0 + m.npages > cap_ || m.npages == 0) { ++bad_man_; break; }   // 越界 ⇒ 丢弃 + 计数
      runs_.emplace(m.seq, make_run(m.page0, m.npages));`
  },
  old: [
    { t: '清单项 `RUN_ADD page0=40 npages=2134219794`。', rows: [{ n: '清单项', items: ['m:npages=2.1e9:bad'] }] },
    { t: '照着分配 ⇒ 9 GB ⇒ 挂载崩溃。', kv: { 'mount': 'bad_alloc ✗' }, ok: false },
  ],
  new: [
    { t: '`page0 + npages > cap_` ⇒ 丢弃，`bad_man_++`；挂载继续。', rows: [{ n: '清单项', items: ['m:丢弃（计数）:del'] }], kv: { 'mount': 'OK ✓', 'bad_man_': 1 }, ok: true },
  ],
});

PIT({
  id: 27, round: 'v2', cat: '压实判据 / 存活账', found: 'v2 空间放大卡 3×', tags: ['live_bytes 上界', 'Bloom', 'Σ cell->live', '空间放大'],
  sym: '垃圾越多越不肯重写，空间放大卡在 3× 不动。',
  cause: '`run->live_bytes` 是合并时用布隆算的**上界**（只会多算）；拿它判"这条 run 值不值得重写"，垃圾多的 run 看起来反而"很活"。',
  rule: '**"脏不脏"用精确账**：`Σ cell->live`（精确、`verify` 每轮核对），保守估计只能用来做"不做"的方向判断（跳过），不能做"做"的方向判断。',
  fix: '压实受害者选择改用单元精确 live；布隆只做读路径过滤。',
  verify: '空间放大 3× → 收敛到目标（后续 `FCS_GC_RATIO` 可调）。',
  cost: '单元精确账需维护（坑 15 的 recount / 增量账）。',
  lesson: '一个只会偏向一边的估计值，用在决策的哪一侧要想清楚。',
  rel: [8, 15, 30],
  code: { kind: 'sketch', src: '按 §12 第 27 条复原',
    old: `- double dirty = 1.0 - (double)r->live_bytes / r->bytes;   // live_bytes 是上界 ⇒ dirty 被低估
- if (dirty < 0.5) skip;                                     // 垃圾越多越不重写`,
    new: `! u64 live = 0; for (Cell* c : cells_of(r)) live += c->live;   // 精确
  double dirty = 1.0 - (double)live / r->bytes;
  if (dirty < FCS_GC_RATIO) skip;`
  },
  old: [
    { t: 'run 真实 live 30%，布隆上界算出 80%。', kv: { 'live_bytes（上界）': '80% ✗', '真实': '30%' } },
    { t: 'dirty=20% < 50% ⇒ 不重写。所有 run 都如此 ⇒ 空间放大 3× 不动。', kv: { '空间放大': '3× ✗' }, ok: false },
  ],
  new: [
    { t: '`Σ cell->live` = 30% ⇒ dirty 70% ⇒ 重写。', kv: { 'dirty': '70% ✓', '空间放大': '收敛 ✓' }, ok: true },
  ],
});

PIT({
  id: 28, round: 'v2', cat: '空间 / compact_file', found: 'v2 compact_file 结果不紧', tags: ['归一两次', 'used = 3×live', 'live + 4 页'],
  sym: '`compact_file` 之后 `used` 停在 3×live。',
  cause: '第一次归一时旧段还占着低位，合并出来的 run 只能落在高处；收完一遍才腾出低位。',
  rule: '**归一两次**：第一次把活数据合成少数 run（落高处），释放旧段；第二次把这些 run 整块搬到最前面。',
  fix: '`compact_file` = 归一 → 归一 → checkpoint → shrink。',
  why: '不做"原地搬"：原地搬要求目标区域已空，第一轮正是在腾空。',
  verify: '`used ≈ live + 4` 页（清单 + SB）。',
  cost: '两倍重写量（compact_file 是离线操作，可接受）。',
  lesson: '压缩文件是两阶段问题：先把东西合起来，再把合起来的东西挪到前面。',
  rel: [17, 22, 'r14-5'],
  code: { kind: 'sketch', src: '按 §12 第 28 条复原',
    old: `- RC compact_file() { normalize(); checkpoint(); shrink(); }     // used 停在 3×live`,
    new: `! RC compact_file() { normalize(); normalize(); checkpoint(); shrink(); }   // 第二次归一把 run 整块搬到最前面`
  },
  old: [
    { t: '初态：低位被旧段 A/B/C 占着（各含部分活数据）。', rows: [{ n: '文件', items: ['a:A:data', 'b:B:data', 'c:C:data', 'h:洞:hole'] }], kv: { 'used': '3×live' } },
    { t: '第一次归一：产物 P 只能落高处；A/B/C 释放。`shrink` 止于 P 的末尾。', rows: [{ n: '文件', items: ['h:洞（原 A/B/C）:hole', 'p:P:new'] }], kv: { 'used': '3×live ✗' }, ok: false },
  ],
  new: [
    { t: '第二次归一：低位全空，P 整块搬到最前面。', rows: [{ n: '文件', items: ['p:P:good', 'h:洞:hole'] }] },
    { t: 'checkpoint + shrink ⇒ `used ≈ live + 4` 页。', rows: [{ n: '文件', items: ['p:P:good', 'm:清单:man', 's:SB:sb'] }], kv: { 'used': 'live + 4 页 ✓' }, ok: true },
  ],
});

PIT({
  id: 29, round: 'v2', cat: '页缓存 / 指针生命周期', found: 'v2 随机坏清单（真凶）+ exact_live_recs 恒 0', tags: ['page()', '直映缓存', 'cache_[pid % N]', '跨页拷出'],
  sym: '`replay_manifest` 在清单页里"读到一条来自数据页的记录"；`exact_live_recs` 存活数恒为 0。',
  cause: '`page(pid)` 把页拷进 `cache_[pid % N]` 并返回它的首地址。遍历某页时再读别的页，若两者映到同一槽，手里的指针会**原地变成另一页的内容**。',
  rule: '**页指针活不过一次 `page()` 调用**：跨页操作必须先把当前页拷出来（或用独立缓冲）。',
  fix: '`replay_manifest` / `exact_live_recs` 等跨页路径先 `memcpy` 当前页到栈缓冲；`page()` 本身保持即取即用的契约并写进注释。',
  why: '不改成"每页独立缓冲 / 引用计数"：直映缓存是嵌入式内存预算下的刻意选择；契约比机制便宜。',
  verify: '随机坏清单消失；`exact_live_recs` 恢复正确；后续所有新读路径遵守"即取即用"（见第 11~14 轮多处注释）。',
  cost: '跨页路径多一次 4 KB 拷贝。',
  lesson: '返回内部缓冲指针的 API 必须写清楚生命周期，而且调用方要被审计。',
  rel: [25, 26, 92],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `page()`（约 2976 行）；老代码按 §12 第 29 条复原',
    old: `  const u8* mp = page(man_pid);                 // 指向 cache_[man_pid % N]
  for (每条清单项 m in mp) {
!     const u8* dp = page(m.page0);              // 若 m.page0 % N == man_pid % N ⇒ mp 原地变成数据页
      ... 继续用 mp 解析下一条 ⇒ "清单页里读到数据页的记录"
  }`,
    new: `  // page(pid)：返回缓存槽的地址 —— 指针只在下一次 page() 调用前有效（即取即用）。
  // v2 是直映 cache_[pid % N]；现在是两路组相联（pid % (cache_n_/2) 选组），契约不变。
  const u8* page(u32 pid) {
      if (pid >= cap_) return nullptr;
      if (!wait_page(pid)) return nullptr;      // 等到超时：当作读失败（明确 RC，不挂死）
      Tls& t = tls();
      const u32 ns = cache_n_ / 2, si = pid % ns;
      CacheEnt* w = &cache_[2 * si];
      ...
  }
+ // 跨页路径（replay_manifest / exact_live_recs …）：先把当前页拷到自己的缓冲再去读别的页
+ std::vector<u8> mbuf(ps_); std::memcpy(mbuf.data(), page(man_pid), ps_);
  for (每条清单项 m in mbuf) { const u8* dp = page(m.page0); ... }`
  },
  old: [
    { t: '缓存 N=4 槽。读清单页 pid=5 → 槽 1；指针 mp 指向槽 1。', rows: [{ n: 'cache_', items: ['s0:槽0:free', 's1:槽1 = 页5（清单）:man', 's2:槽2:free', 's3:槽3:free'] }] },
    { t: '解析某项时读数据页 pid=9 → 9%4=1 ⇒ **槽 1 被覆盖**；mp 仍指槽 1，现在是数据页内容。', rows: [{ n: 'cache_', items: ['s0:槽0:free', 's1:槽1 = 页9（数据）← mp:bad', 's2:槽2:free', 's3:槽3:free'] }], kv: { '下一条清单项': '来自数据页 ✗' }, log: ['!apply_man: npages=2134219794 (bogus)'], ok: false },
  ],
  new: [
    { t: '先把清单页拷到 `mbuf`，再读数据页；槽 1 被覆盖无所谓。', rows: [{ n: 'cache_', items: ['s1:槽1 = 页9:data'] }, { n: 'mbuf（栈）', items: ['m:页5 副本 ← 解析:good'] }], kv: { '清单项': '✓' }, ok: true },
  ],
});

PIT({
  id: 30, round: 'v2', cat: 'Bloom / 存活账', found: 'v2 同一负载 0.95 s → 16.29 s', tags: ['Bloom', 'u32 截断', 'may() 恒假', '17×'],
  sym: '不是读错数据，而是**慢 17×**：同一负载 0.95 s → 16.29 s；读页 8 万 → 20 万；run 越堆越多。',
  cause: '`Bloom::add` 用完整 64 位哈希落 4 个位，查询侧写成 `u32 hk = khash(k)`——截成 32 位，`may()` 几乎恒 false。后果是**存活账永远算满**：零写回收从不触发、分层合并觉得"没垃圾"。',
  rule: '布隆是**保守过滤器**：错误方向只会让最优路径失效，不会报错。所以 (a) add / may 必须共享同一个哈希类型；(b) 存活账必须在回收前用精确复核兜底（坑 31）。',
  fix: '`may(u64 h)` 与 `add(u64 h)` 同签名；空布隆 `may` 返回 true。',
  verify: '0.95 s 恢复；读页回到 8 万量级。',
  cost: '无。',
  lesson: '"只影响性能不影响正确性"的组件，要用性能回归守，否则悄悄退化。',
  rel: [27, 31],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` `Bloom::add/may`（约 320 / 330 行）；老代码按 §12 第 30 条复原',
    old: `  void add(u64 h) { for 4 位: set(h >> (16*i)); }
- bool may(u64 k) { u32 hk = (u32)khash(k); ... test(hk >> (16*i)) ... }   // 高 32 位丢了 ⇒ 位置对不上 ⇒ 恒 false`,
    new: `  void add(u64 h) {
      if (!bits) reset(8);
      u32 a, b, c, d;
      pos(h, bits, a, b, c, d);                 // 4 个位置都从同一个 64 位 h 算（最高位段 h >> 48）
      w[a >> 6] |= 1ull << (a & 63); ... ++nk;
  }
! bool may(u64 h) const {                       // 与 add 同一个 u64 h、同一个 pos()
      if (!bits) return true;
      u32 a, b, c, d;
      pos(h, bits, a, b, c, d);
      return ((w[a >> 6] >> (a & 63)) & 1u) && ((w[b >> 6] >> (b & 63)) & 1u) && ...;
  }`
  },
  old: [
    { t: '`add(h64)` 落位；`may((u32)h)` 查别的位置 ⇒ false。', rows: [{ n: 'bits', items: ['b1:add 落位 ✓:data', 'b2:may 查错位 ✗:bad'] }], kv: { 'may()': '恒 false' } },
    { t: '"更新副本不存在" ⇒ 每条都算活 ⇒ 回收不触发 ⇒ run 堆积。', lvl: { L0: ['r:run ×40:bad'] }, kv: { '读页': '8 万 → 20 万', '耗时': '0.95 → 16.29 s ✗' }, ok: false },
  ],
  new: [
    { t: 'add / may 同一哈希 ⇒ 过滤正确；存活账正常递减；回收 / 合并触发。', lvl: { L0: ['r:run ×3:good'] }, kv: { '耗时': '0.95 s ✓' }, ok: true },
  ],
});

PIT({
  id: 31, round: 'v2', cat: '回收安全', found: '坑 30 的兜底', tags: ['retire_dead_runs', 'exact_live_recs', '精确复核'],
  sym: '（预防）增量计数若偏低会误删活数据——最可怕的一条路。',
  cause: '`retire_dead_runs` 判"整条 run 全死"若只听增量计数，计数错一次就丢数据。',
  rule: '**回收前必须精确复核**：计数是提示值（只会偏低 ⇒ 只会"误以为还活"），`exact_live_recs()` 扫一遍确认才退休。',
  fix: '`if (r->live_recs == 0 && exact_live_recs(r) == 0) retire(r)`。',
  why: '代价是每轮多扫一遍待回收 run，但把"误删"从可能变成不可能；第 11 轮进一步把扫描量缩到被引用的 span（坑 92）。',
  verify: 'v2 起所有回收路径；第 11 轮坑 92 保持同一口径。',
  cost: '每轮扫一遍候选 run。',
  lesson: '破坏性操作前用独立口径复核，是对"账可能错"的唯一诚实回应。',
  rel: [30, 92, 63],
  code: { kind: 'sketch', src: '按 §12 第 31 条复原',
    old: `- if (r->live_recs == 0) retire(r);                // 只听增量计数`,
    new: `! if (r->live_recs == 0 && exact_live_recs(r) == 0) retire(r);   // 精确复核兜底`
  },
  old: [
    { t: '计数因某处漏加显示 0，实际还有 2 条活记录。', kv: { 'live_recs': '0（错）', '真值': 2 } },
    { t: '直接退休 ⇒ 活数据丢失。', kv: { 'get': 'NOTFOUND ✗' }, ok: false },
  ],
  new: [
    { t: '`exact_live_recs` 扫出 2 ⇒ 不退休；计数被校准。', kv: { 'exact': '2 ✓', '退休': '否' }, ok: true },
  ],
});

PIT({
  id: 32, round: 'v2', cat: '回退 / 逻辑时钟', found: 'r2 阶段 4', tags: ['rollback', 'vers_', '双槽 SB', '静默复活'],
  sym: '回退之后下次挂载，被回退的分支**静默复活**。',
  cause: '`rollback` 把 `vers_` 恢复到快照点；另一个超级块槽里"版本更高的旧分支"在挂载时胜出。',
  rule: '**回退的是状态，不是历史**：逻辑时钟必须继续前进——`vers_ = 回退前的时钟 + 1`。',
  fix: '`rollback()` 末尾 `vers_ = max(vers_, old_vers) + 1`，写 SB。',
  verify: 'r2 阶段 4：回退 → 重开 → 回退态保持。',
  cost: '无。',
  lesson: '单调时钟在任何"选最新"的协议里都不能倒退，哪怕语义上是在"回去"。',
  rel: [33, 25, 103],
  code: { kind: 'sketch', src: '按 §12 第 32 条复原',
    old: `  RC rollback() { ... restore_snapshot();
!     vers_ = snap.vers;              // 时钟倒退 ⇒ 另一槽的旧分支（版本更高）挂载时胜出`,
    new: `  RC rollback() { u64 before = vers_; ... restore_snapshot();
!     vers_ = std::max(before, snap.vers) + 1;    // 回退的是状态，不是历史
      write_sb();`
  },
  old: [
    { t: '槽 A: SB v12（当前分支），槽 B: SB v11。回退到快照 v9：`vers_=9`，写 SB v9 到槽 B。', rows: [{ n: 'SB 槽', items: ['a:A v12（被回退分支）:sb', 'b:B v9（回退态）:sb'] }] },
    { t: '挂载选版本高者 ⇒ A v12 胜 ⇒ 回退被静默撤销。', kv: { 'mount 选中': 'v12 ✗' }, ok: false },
  ],
  new: [
    { t: '回退态写 SB v13（= 12 + 1）。', rows: [{ n: 'SB 槽', items: ['a:A v12:sb', 'b:B v13（回退态）:good'] }] },
    { t: '挂载选 v13 ⇒ 回退态保持。', kv: { 'mount 选中': 'v13 ✓' }, ok: true },
  ],
});

PIT({
  id: 33, round: 'v2', cat: '回退 / 回收账', found: 'r2 回退 + 双重分配', tags: ['pending_free_', 'deferred_', '快照整体保存', '同页分配两次'],
  sym: '回退后同一页被**分配两次**。',
  cause: '`pending_free_` / `deferred_` 里装的是"还没归位"的页；回退不还原它们，随后 `promote_pending()` 把属于**被回退分支**的页放进空闲表。',
  rule: '**快照必须包含回收账**；回退按内容整体还原 `pending_free_` / `deferred_`。',
  fix: '快照结构加 `s.pending_free = pending_free_; s.deferred = deferred_;`，回退时整体赋回。',
  verify: 'r2 回退段；g6 掉电 + 回退种子。',
  cost: '快照多存两个向量。',
  lesson: '"状态"的定义要完整：任何会影响未来分配决策的容器都是状态。',
  rel: [5, 32, 99],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` 快照结构赋值（约 2517 行）；老代码按 §12 第 33 条复原',
    old: `  Snap take() { Snap s; s.cells = cells_; s.runs = runs_; s.free = free_; return s; }
- // pending_free_ / deferred_ 没进快照 ⇒ 回退后 promote 把旧分支的页放进 free_`,
    new: `  Snap take() { Snap s; ... s.free = free_;
!     s.pending_free = pending_free_; s.deferred = deferred_;      // 回收账一起存
      return s; }
  RC rollback() { ... pending_free_ = s.pending_free; deferred_ = s.deferred; ... }`
  },
  old: [
    { t: '快照 S 时 pending_free_=∅。之后分支 X 写页 30，又让页 30 进 pending_free_（X 自己退休了它）。', rows: [{ n: 'pending_free_', items: ['p:30（属于分支 X）:pend'] }] },
    { t: '回退到 S：cells/runs/free 还原，但 pending_free_ 仍含 30。promote ⇒ 30 进 free_。可是 S 的世界里 30 可能已被别的 run 占用 ⇒ 分配两次。', rows: [{ n: 'free_', items: ['f:30:bad'] }, { n: 'S 中页 30', items: ['r:run Y 的页:data'] }], kv: { '页 30': '双重分配 ✗' }, ok: false },
  ],
  new: [
    { t: '快照存了 pending_free_=∅；回退整体赋回 ⇒ 30 不会被 promote。', rows: [{ n: 'pending_free_', items: ['p:∅:free'] }, { n: 'S 中页 30', items: ['r:run Y 的页 ✓:good'] }], ok: true },
  ],
});

PIT({
  id: 34, round: 'v2', cat: '合并 / 存活账', found: 'v2 热点覆盖负载：118 活 / 真值 40', tags: ['merge', '(seq, page0)', 'finish_run', '事后反推'],
  sym: '热点覆盖负载里一条 run 记 118 条活、真值 40 ⇒ 永远不满足"全死可回收"，空间只涨不降。',
  cause: '合并后的 run 只有一个 seq（= 源里最新的）；拿它去问"这条记录有没有更新副本"会把从旧源搬来的记录算成"新"的（比它们原来的 seq 新）。',
  rule: '判据必须用**产物自己的段身份 `(seq, page0)`**，而且要等 `finish_run` 分配完页号之后再判（`(seq, rid)` 才是读路径的全序）。',
  fix: '合并产物的 live 在 `finish_run` 之后、以 `seg_key(seq, page0)` 为身份、按单元归并口径算。',
  verify: '118 → 40；空间收敛。',
  cost: '无。',
  lesson: '"身份"包含页号，而页号在写完之前不存在——任何以身份为前提的计算都要排在 `finish_run` 后面。',
  rel: [9, 10, 90, 15],
  code: { kind: 'sketch', src: '按 §12 第 34 条复原',
    old: `- // 合并后：拿产物 seq 反推每条记录是否有更新副本
- for (rec : nr) if (!newer_than(rec.key, nr->seq)) ++nr->live_recs;   // 旧源搬来的记录被算成"新" ⇒ 118`,
    new: `  finish_run(nr);                                           // 先拿到 page0
! u64 id = seg_key(nr->seq, nr->page0);                     // 产物自己的段身份
! nr->live_recs = count_live_by_cells(id);                  // 按单元归并口径 ⇒ 40`
  },
  old: [
    { t: '源 R1(seq 2) R2(seq 5)；产物 P 取 seq 5。R1 搬来的 78 条老记录在 R2 里有更新副本（已经在归并中被丢？不——这里问的是其它活 run）：以 seq 5 反推时，它们比原 seq 2 "新"了。', kv: { 'live_recs（反推）': '118 ✗', '真值': 40 } },
    { t: '永不满足全死 ⇒ 空间只涨不降。', kv: { '空间': '↑ ✗' }, ok: false },
  ],
  new: [
    { t: '`finish_run` 后以 `(seq 5, page0)` 为身份、按单元口径数 ⇒ 40。', kv: { 'live_recs': '40 ✓' }, ok: true },
  ],
});

PIT({
  id: 35, round: 'v2', cat: '切分 / 存活账', found: 'verify: run live recs = 2×nrec', tags: ['segs.swap()', 'seg_set_live', 'recount', '加两遍'],
  sym: '`live_recs` 恰好是 nrec 的 **2 倍**，`verify` 的 `run live recs` 报错。',
  cause: '段级记账里 `run->live_recs` 是它在所有单元里的 live 之和。切分用 `segs.swap()` 换掉整张表，原 `Seg` 连同计数一起被丢掉而没"退回"；随后父子各 `recount` 一次又加一遍。',
  rule: '**丢弃段之前先把它贡献的计数退回**（`seg_set_live(sg, 0)`）；任何"替换整张表"的操作都要先对旧表做一次清零。',
  fix: '`split()` 在 swap 前遍历旧 segs `seg_set_live(sg, 0)`。',
  verify: '`verify` 不再报；g 系列切分段。',
  cost: '切分时 O(段) 清零。',
  lesson: '聚合计数（Σ）的每个贡献者都要有对称的加 / 减路径；批量替换最容易漏掉减。',
  rel: [1, 15, 34],
  code: { kind: 'sketch', src: '按 §12 第 35 条复原',
    old: `  void split(Cell* c, u64 mid) {
      std::map<u64, Seg> ls, rs; ... 填 ls / rs ...
!     c->segs.swap(ls);                 // 旧 Seg 连同其 live 计数被丢掉，没退回 run->live_recs
      recount(c); recount(r);           // 再加一遍 ⇒ 2×`,
    new: `  void split(Cell* c, u64 mid) {
+     for (auto& kv : c->segs) seg_set_live(kv.second, 0);    // 先退回旧段贡献
      std::map<u64, Seg> ls, rs; ...
      c->segs.swap(ls);
      recount(c); recount(r);`
  },
  old: [
    { t: 'run R：nrec=10，段 S 贡献 live 10 ⇒ `R.live_recs=10`。', kv: { 'R.live_recs': 10 } },
    { t: '切分 swap 丢掉 S（未退回）；父子 recount 各加 5 ⇒ 20。', kv: { 'R.live_recs': '20 ✗' }, log: ['!verify: run live recs 20 vs 10'], ok: false },
  ],
  new: [
    { t: 'swap 前 `seg_set_live(S, 0)` ⇒ 0；recount 加回 5+5 ⇒ 10。', kv: { 'R.live_recs': '10 ✓' }, ok: true },
  ],
});

PIT({
  id: 36, round: 'v2（第 10 轮后成历史）', cat: '压实 / seq', found: 'v2 账漂移', tags: ['compact_cell', 'seq 取用时机', '撞号', '坑 90'],
  sym: '压实产物和一条还活着的 run **撞号**，`(seq, rid)` 胜负随手一翻就变，账跟着漂。',
  cause: '`compact_cell` 原来在释放源 run 之后才去 `runs_` 里找它们的 seq / lvl；删掉的自然找不到，拿到的是一条无关 run 的 seq。',
  rule: '**产物的 seq 要在删除源 run 之前取**；更根本地（第 10 轮，坑 90）：压实产物不再沿用源的 seq，而是取新鲜 seq 进 L0。',
  fix: 'v2：先读源 seq 再释放；第 10 轮后：`nr->seq = next_seq_++`。',
  verify: 'v2 回归；第 10 轮 g10。',
  cost: '无。',
  lesson: '"先删后查"是使用后释放（use-after-free）的逻辑版；顺序问题最好用更强的规则（新鲜 seq）彻底消掉。',
  rel: [3, 9, 90],
  code: { kind: 'sketch', src: '按 §12 第 36 条复原；第 10 轮后由坑 90 的规则取代',
    old: `  release_sources(srcs);                      // 先删
- u32 seq = 0; for (RunPtr r : srcs) seq = std::max(seq, runs_.at(r->seq)->seq);   // 后查：找不到 ⇒ 拿到无关 run 的 seq`,
    new: `  // v2 修法：先取后删
+ u32 seq = max_seq_of(srcs); release_sources(srcs);
  // 第 10 轮后（坑 90）：nr->seq = next_seq_++; nr->lvl = 0;   // 新鲜 seq 进 L0，彻底不与任何活 run 相同`
  },
  old: [
    { t: '源 R1(seq 4) R2(seq 6)；先释放，再从 `runs_` 找 seq ⇒ 找不到 ⇒ 落到无关的 R9(seq 9)。', rows: [{ n: 'runs_', items: ['r1:R1 已删:del', 'r2:R2 已删:del', 'r9:R9 seq9（活）:data'] }], kv: { '产物 seq': '9 ✗（撞 R9）' } },
    { t: '产物 P(seq 9) 与 R9 同号：`(seq,rid)` 比较只剩 rid，胜负随页号翻转 ⇒ 账漂。', kv: { 'get': '不稳定 ✗' }, ok: false },
  ],
  new: [
    { t: '先取 max(4,6)=6 再释放（v2）；第 10 轮后直接取新鲜 seq 10 进 L0。', rows: [{ n: 'runs_', items: ['p:P seq10 L0:good', 'r9:R9 seq9:data'] }], kv: { '产物 seq': '唯一 ✓' }, ok: true },
  ],
});
