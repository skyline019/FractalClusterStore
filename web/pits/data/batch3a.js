// 第 3 批（a）：坑 37 ~ 52（第六批「容量 / 大值 A2 / 溢出链」→ 第七批「快照正确性 + 链接管」）。
// 无历史头：老代码按 §12 复原（sketch）；能从当前 include/fcs.h 原样引用的新代码标 half。数字来自 §12 当年实测。

PIT({
  id: 37, round: '第六批（容量）', cat: '容量 / 静默截断', found: 'PageId 16 → 32 位改造', tags: ['sp.packed', '页<<16', '页<<12', '静默'],
  sym: '超过 65535 页之后 `get` 开始返回**别的键的值**——不报错、不崩。',
  cause: '`packed = 页<<16 | 偏移`：页号 > 65535 时高位被截掉，变成"余数页"。读到的是一张合法页，CRC 也对，只是不是要的那张。',
  rule: '**容量改造的顺序不能反**：先改打包格式（`页<<12 | 偏移`，20 位页号，`static_assert(PAGE <= 1<<12)`），再放宽 `PageId`。',
  fix: '`SP_SHIFT = 12`，页内偏移 12 位、页号 20 位；`static_assert` 守着页大小。',
  why: '反过来做的话中间态"一切正常"，直到库长过 65535 页才静默出错——最坏的一类 bug。',
  verify: '容量回归 200k 页；`static_assert` 编译期守。',
  cost: '页号上限 2^20 = 100 万页（4 GB @ 4 KB），满足嵌入式目标。',
  lesson: '位域打包的每个字段都要有编译期断言，改宽度先改断言。',
  rel: [38, 22],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @93 `static_assert`；老代码按 §12 第 37 条复原',
    old: `- u32 packed = (pid << 16) | off;        // pid > 65535 ⇒ 高位静默丢失 ⇒ 读到"余数页"`,
    new: `  static_assert(PAGE <= (1u << 12), "SP_SHIFT 要能装下页内偏移");
! u32 packed = (pid << 12) | off;        // 20 位页号 + 12 位偏移`
  },
  old: [
    { t: '页号 70000 = 0x11170。`<<16` 后高位丢失，解包得 70000 & 0xFFFF = 4464。', rows: [{ n: 'packed', items: ['p:pid 70000 → 4464:bad'] }], kv: { '读到的页': '4464（别人的）' } },
    { t: 'CRC 对、格式对，只是键不对 ⇒ 返回别的键的值。', kv: { 'get(k)': '错值 ✗（静默）' }, ok: false },
  ],
  new: [
    { t: '`<<12`：70000 完整保留（20 位）。', rows: [{ n: 'packed', items: ['p:pid 70000 ✓:good'] }], kv: { 'get(k)': '✓' }, ok: true },
  ],
});

PIT({
  id: 38, round: '第六批（容量）', cat: '容量 / 类型宽度', found: 'RamDev(200000) 变 3392 页', tags: ['PageId=uint16_t', '隐式截断', 'NOSPACE 误判'],
  sym: '`RamDev(200000)` 实际只有 3392 页；`RamDev(65536)` 是 **0 页**。当年被误判成"元数据页把设备占满了"。',
  cause: '`PageId = uint16_t` 时，"设备容量"这个入参也被隐式截成 `200000 & 0xFFFF`。',
  rule: '**任何"页数"参数都必须先确认类型够宽**（`u32`），别等写到一半才 NOSPACE。',
  fix: '`PageId`/容量/页数全部 `u32`；设备构造加断言。',
  verify: '容量回归；`FileDev(path, npages, psize, create)` 的 npages 为 `u32`。',
  cost: '无。',
  lesson: '窄类型的污染会沿着参数链传播到看似无关的地方；排查时先怀疑类型。',
  rel: [37, 22],
  code: { kind: 'sketch', src: '按 §12 第 38 条复原',
    old: `- using PageId = uint16_t;
- RamDev(PageId npages);            // RamDev(200000) ⇒ 3392；RamDev(65536) ⇒ 0`,
    new: `+ using PageId = uint32_t;
  RamDev(u32 npages);               // 入参不再被截`
  },
  old: [
    { t: '`RamDev(200000)`：200000 & 0xFFFF = 3392。', rows: [{ n: '设备', items: ['d:3392 页:bad'] }], kv: { '期望': 200000 } },
    { t: '很快 NOSPACE，看起来像"元数据占满"。', kv: { '写入': 'NOSPACE ✗' }, ok: false },
  ],
  new: [
    { t: '`u32` ⇒ 200000 页。', rows: [{ n: '设备', items: ['d:200000 页:good'] }], ok: true },
  ],
});

PIT({
  id: 39, round: '第六批', cat: '容器 / 迭代器', found: 'heal 返回 0', tags: ['heal_missing_meta', '边遍历边 erase', '先收集再处理'],
  sym: '`heal_missing_meta` 返回 0——看起来像"没找到需要补的 run"，其实**一个候选都跳过了**。',
  cause: '第一遍收集候选时顺手 `runs_.erase`，第二遍 `begin()` 用到了已失效的 `end()` 快照。',
  rule: '**遍历容器时不改它**：先收集 `page0` 列表，再单独循环处理。',
  fix: '两阶段：收集 → 处理。',
  verify: 'heal 回归：缺 meta 的 run 全部补齐。',
  cost: '一个临时向量。',
  lesson: '"顺手 erase"是 C++ 容器里最常见的静默错误，哪怕没崩也可能静默少做。',
  rel: [41, 40],
  code: { kind: 'sketch', src: '按 §12 第 39 条复原',
    old: `- for (auto it = runs_.begin(); it != end_snapshot; ++it) {
-     if (missing_meta(it->second)) { cand.push_back(...); runs_.erase(it); }   // 迭代器失效
- }`,
    new: `+ std::vector<u32> cand; for (auto& kv : runs_) if (missing_meta(kv.second)) cand.push_back(kv.first);
+ for (u32 p0 : cand) heal_one(p0);     // 先收集，再处理`
  },
  old: [
    { t: '3 条 run 缺 meta。第一条 erase 后迭代器失效，后续比较失真。', rows: [{ n: 'runs_', items: ['a:A 缺:data', 'b:B 缺:data', 'c:C 缺:data'] }] },
    { t: '循环提前结束 ⇒ heal 返回 0。', kv: { 'healed': '0 ✗' }, ok: false },
  ],
  new: [
    { t: '先收集 [A,B,C]，再逐个补。', kv: { 'healed': '3 ✓' }, ok: true },
  ],
});

PIT({
  id: 40, round: '第六批（A2 大值）', cat: '页缓存 / 指针生命周期', found: 'verify recs 2000 → 2918、run unsorted', tags: ['read_ovfl', '页指针失效', '16 B 描述符先拷出'],
  sym: '`verify` 报 `recs` 2000 → 2918、`run unsorted`：溢出页的内容被当成数据页解析。',
  cause: '`verify` 在"逐页解析记录"的循环里调 `read_ovfl`（它会踢页缓存），之后继续用同一个 `p`——坑 29 的第 N 次咬人。',
  rule: '**页指针只活到下一次 `page()`**：先把 16 B 描述符 `memcpy` 到局部，读完链再 `p = page(run->page0 + i)` 重新定位。',
  fix: '描述符先拷出；读链后重新取页。',
  verify: '`verify` 恢复 2000；坑 56 在 `scrub` 里又踩一次后同样修。',
  cost: '一次 16 B 拷贝 + 一次重取页（缓存命中）。',
  lesson: '新加的读路径（大值）要重新审计所有"跨页"调用点。',
  rel: [29, 56, 64],
  code: { kind: 'sketch', src: '按 §12 第 40 条复原',
    old: `  const u8* p = page(r->page0 + i);
  for (每条记录) {
!     if (len == OVFL_MARK) read_ovfl(desc_of(val_ptr), buf);   // 踢掉页缓存 ⇒ p 失效
      ... 继续用 p 解析下一条 ⇒ 溢出页内容当数据页`,
    new: `  const u8* p = page(r->page0 + i);
  for (每条记录) {
      if (len == OVFL_MARK) {
+         u8 d[16]; std::memcpy(d, val_ptr, 16);              // 先拷出描述符
          read_ovfl(desc_of(d), buf);
!         p = page(r->page0 + i);                             // 重新定位
      }`
  },
  old: [
    { t: '解析数据页 D 到第 3 条（大值）→ `read_ovfl` 读链页 O ⇒ 缓存槽被 O 占据。', rows: [{ n: '缓存槽', items: ['s:页 D → 页 O:bad'] }] },
    { t: '继续用 p：把 O 的字节当记录解析 ⇒ recs 2918、unsorted。', kv: { 'recs': '2918 ✗', 'verify': 'run unsorted' }, ok: false },
  ],
  new: [
    { t: '描述符先拷出，读链后 `p = page(...)` 重新定位 ⇒ 解析继续在 D 上。', rows: [{ n: '缓存槽', items: ['s:页 D（重取）:good'] }], kv: { 'recs': '2000 ✓' }, ok: true },
  ],
});

PIT({
  id: 41, round: '第六批', cat: '容器 / UAF', found: 'ASan', tags: ['map::erase', 'it->second', 'use-after-free'],
  sym: 'ASan 直接抓到 use-after-free；普通构建下只是"偶尔算出个荒唐的剩余块长度"。',
  cause: '`free_.erase(it); if (it->second > mn) ...`——erase 之后用 `it`。',
  rule: '**先取值再 erase**：`const u32 blk = it->second;`。',
  fix: '如右。',
  verify: 'ASan 全绿。',
  cost: '无。',
  lesson: 'ASan 是常开项（`make asan`），不是出事才跑。',
  rel: [39],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @2352；老代码按 §12 第 41 条复原',
    old: `- free_.erase(it);
- if (it->second > mn) ...            // it 已失效`,
    new: `! const u32 blk = it->second;            // erase 之后 it 就失效了，必须先存
  free_.erase(it);
  if (blk > mn) ...`
  },
  old: [
    { t: 'erase 后读 `it->second`：读到释放的内存。', kv: { 'blk': '随机 ✗', 'ASan': 'heap-use-after-free' }, ok: false },
  ],
  new: [
    { t: '先存 `blk` 再 erase。', kv: { 'blk': '✓', 'ASan': '干净' }, ok: true },
  ],
});

PIT({
  id: 42, round: '第六批（A2 大值）', cat: '大值 / 所有权', found: 'A2 flush 双写链', tags: ['put_raw', 'Rec.ovfl', 'add_desc', '孤儿链'],
  sym: '每个大值落盘**两条链**，其中一条成了孤儿（没有任何描述符引用）。',
  cause: '`put_raw` 在 `put` 时就把链写好并留下 16 B 描述符；`flush_mem` 若再走 `w.add()`（按值长度判断）就会再写一条链。',
  rule: '**内存表里的大值只落一次链**：`Rec` 带 `bool ovfl`，flush 时走 `add_desc()`（所有权转移，不复制值）。',
  fix: '`Rec.ovfl`；`flush_mem` 里 `r.ovfl ? w.add_desc(key, ver, desc) : w.add(key, ver, val)`。',
  verify: 'A2 回归：链数 = 大值数；`ownership_check` 无孤儿。',
  cost: '无。',
  lesson: '一个对象在两条路径上都"会写盘"时，要明确谁拥有、谁只是转交。',
  rel: [45, 46, 73],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @187 `bool ovfl`、@5340 flush 分支；老代码按 §12 第 42 条复原',
    old: `  // flush_mem
- bool ok = w.add(r.key, r.ver, val, len);   // len 大 ⇒ RunWriter 自己再写一条链 ⇒ 描述符那条成孤儿`,
    new: `  struct Rec { ...; bool ovfl = false; };   // 大值：值区里是 16 B 溢出描述符（真值在溢出页链里）
  // flush_mem
! bool ok = r.ovfl ? w.add_desc(r.key, r.ver, mem_.val(r))   // 所有权转移：只写描述符
                   : w.add(r.key, r.ver, val, len);`
  },
  old: [
    { t: '`put` 大值 ⇒ 链 C1 + 描述符。', rows: [{ n: '页', items: ['c1:链 C1:data', 'd:描述符→C1:man'] }] },
    { t: '`flush_mem` 按长度又写链 C2，run 里的描述符指向 C2 ⇒ C1 孤儿。', rows: [{ n: '页', items: ['c1:链 C1（孤儿）:bad', 'c2:链 C2:new', 'r:run 描述符→C2:data'] }], kv: { '链数': '2 ✗' }, ok: false },
  ],
  new: [
    { t: '`add_desc` 只写描述符，链 C1 归 run。', rows: [{ n: '页', items: ['c1:链 C1:good', 'r:run 描述符→C1:data'] }], kv: { '链数': '1 ✓' }, ok: true },
  ],
});

PIT({
  id: 43, round: '第六批', cat: '提交 / 快路径', found: 'del_range 重开复活', tags: ['commit 早退', 'delta.empty()', 'pend_ranges_'],
  sym: '"只删了一个范围"的提交被当成"没东西可提交"，墓碑永远不落盘——重开后全部复活。',
  cause: '`commit()` 的空提交快路径只判 `delta.empty()`；`del_range` 只改内存里的 `pend_ranges_`。',
  rule: '**"没事可做"的判据必须覆盖所有待持久化状态**：`delta`、`pend_delta_`、`pend_ranges_`（后来还有 `gpend_`、`wr_gen_`）。',
  fix: '`if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty())` 才早退。',
  verify: 'del_range + 重开回归；坑 68 再加 `pend_delta_`。',
  cost: '无。',
  lesson: '每加一种"延迟持久化"的状态，都要回头改早退判据。',
  rel: [44, 68, 69],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1851；老代码按 §12 第 43 条复原',
    old: `- if (delta.empty()) { return RC::OK; }          // 只删范围 ⇒ delta 空 ⇒ 早退 ⇒ 墓碑不落盘`,
    new: `! if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
      promote_pending(); durable_gen_ = wr_gen_; ... return RC::OK;
  }`
  },
  old: [
    { t: '`del_range(10,90)` → `pend_ranges_` 一条；`commit()`：delta 空 ⇒ 早退。', rows: [{ n: 'pend_ranges_', items: ['r:[10,90):tomb'] }, { n: '盘上', items: ['x:无墓碑:bad'] }] },
    { t: '重开 ⇒ 10..89 全部复活。', kv: { 'get(50)': '有值 ✗' }, ok: false },
  ],
  new: [
    { t: '`pend_ranges_` 非空 ⇒ 不早退 ⇒ `M_RANGE` 入清单。', rows: [{ n: '盘上清单', items: ['r:M_RANGE [10,90):tomb'] }], kv: { '重开 get(50)': 'NOTFOUND ✓' }, ok: true },
  ],
});

PIT({
  id: 44, round: '第六批', cat: '清单快照 / 重放顺序', found: '全量清单快照后墓碑复活', tags: ['write_man_snapshot', 'pend_', '内联增量', 'M_RANGE'],
  sym: '写完全量清单快照后，刚清掉的范围墓碑**复活**。',
  cause: '`write_man_snapshot()` 之后没清 `pend_`（SB 内联增量），而内联增量是在清单链**之后**应用的：`M_RUN_ADD` 重放无害，`M_RANGE` 重放把墓碑加回来。',
  rule: '**全量快照之后，所有"增量"都已被快照包含 ⇒ 必须清空**：`pend_.clear()` 再 `write_sb()`。',
  fix: '如右。',
  verify: 'checkpoint + 重开回归。',
  cost: '无。',
  lesson: '两份"事实来源"（快照 + 增量）同时存在时，写快照的那一刻要让增量归零。',
  rel: [43, 68],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1951 / @2056 `pend_.clear()`；老代码按 §12 第 44 条复原',
    old: `  write_man_snapshot();
- write_sb();                      // pend_ 里的 M_RANGE 仍内联在 SB 里 ⇒ 重放时在快照之后再应用一遍`,
    new: `  write_man_snapshot();
! pend_.clear();                   // 快照已含全部状态
  write_sb();`
  },
  old: [
    { t: 'pend_ 含 `M_RANGE [10,90)`；随后墓碑被结算 / 删除；写全量快照（不含墓碑）。', rows: [{ n: 'SB 内联', items: ['r:M_RANGE [10,90):tomb'] }, { n: '快照', items: ['s:无墓碑:man'] }] },
    { t: '重放：快照 → 内联增量 ⇒ 墓碑复活。', kv: { '墓碑': '复活 ✗' }, ok: false },
  ],
  new: [
    { t: '快照后 `pend_.clear()` ⇒ SB 内联为空。', rows: [{ n: 'SB 内联', items: ['e:∅:free'] }, { n: '快照', items: ['s:无墓碑:man'] }], kv: { '重放': '✓' }, ok: true },
  ],
});

PIT({
  id: 45, round: '第六批（A2 大值）', cat: '大值 / 空间', found: 'used 只涨不降', tags: ['free_ovfl_of', 'rebuild_free', '孤儿链'],
  sym: '`verify` 干净，但 `used` 只涨不降。',
  cause: '摘 run 只 `free_pages(page0, npages + mnpg)`，溢出链没释放 ⇒ 孤儿；挂载时 `rebuild_free()` 也没把活 run 的链算作已用 ⇒ 还可能被二次分配。',
  rule: '**链随 run 生死**：摘 run 前 `free_ovfl_of(r)`；`rebuild_free()` 把活 run 的链算作已用。',
  fix: '如右。',
  verify: 'A2 覆盖负载 used 收敛；`ownership_check`。',
  cost: '摘 run 时多一次链遍历。',
  lesson: '新的页引用来源（这里是链）必须同步进"释放"和"重建空闲表"两处（坑 74 后扩成三处）。',
  rel: [42, 57, 74],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @4159 / @4969 `free_ovfl_of`；老代码按 §12 第 45 条复原',
    old: `- free_pages(r->page0, r->npages + r->mnpg);      // 链没释放 ⇒ 孤儿`,
    new: `! free_ovfl_of(dead, safe);                          // 先放链
  free_pages(dead->page0, run_block(dead), safe);
  // rebuild_free()：活 run 的链也标为已用`
  },
  old: [
    { t: 'run R 退休：块释放，链 C 留着。', rows: [{ n: '页', items: ['b:R 块:free', 'c:链 C（孤儿）:bad'] }], kv: { 'used': '↑ ✗' }, ok: false },
  ],
  new: [
    { t: '`free_ovfl_of(R)` 再释放块。', rows: [{ n: '页', items: ['b:R 块:free', 'c:链 C:free'] }], kv: { 'used': '↓ ✓' }, ok: true },
  ],
});

PIT({
  id: 46, round: '第六批（A2 大值）', cat: '大值 / 失败路径', found: 'RunWriter NOSPACE', tags: ['RunWriter::chains', 'abandon()', '白占'],
  sym: '写到第 k 条 NOSPACE：前 k−1 条的链已落盘，但 run 没建成 ⇒ 没有清单项引用它们，盘空间白占到下次挂载。',
  cause: '失败路径不知道本次写过哪些链。',
  rule: '**写者记住自己分配过的一切**：`RunWriter::chains`；失败 `abandon()` 全退。',
  fix: '如右。',
  verify: 'NOSPACE 注入；`rebuild_free()` 仍作为兜底。',
  cost: '一个向量。',
  lesson: '事务式写法：成功才移交所有权，失败自己收拾。',
  rel: [42, 45, 24],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @4326 `abandon()`；老代码按 §12 第 46 条复原',
    old: `  for (rec : mem) if (!w.add(rec)) return RC::NOSPACE;   // 已写的链没人退`,
    new: `  for (rec : mem) if (!w.add(rec)) { w.abandon(); return RC::NOSPACE; }
  struct RunWriter { std::vector<std::pair<u32,u32>> chains; void abandon() { for (auto& c : chains) s->free_pages(c.first, c.second); ... } };`
  },
  old: [
    { t: '第 3 条 NOSPACE；链 C1、C2 已落盘。', rows: [{ n: '页', items: ['c1:C1:bad', 'c2:C2:bad'] }], kv: { '引用者': '无 ✗' }, ok: false },
  ],
  new: [
    { t: '`abandon()` 退 C1、C2。', rows: [{ n: '页', items: ['c1:C1:free', 'c2:C2:free'] }], ok: true },
  ],
});

PIT({
  id: 47, round: '第六批（A2 大值）', cat: '测试方法论', found: '恰好跨页的 off-by-one', tags: ['3000/3001/4080/8160', '边界测试', 'throw/assert'],
  sym: '`throw` 不出细节、`assert` 只在 debug 生效 ⇒ "恰好跨页"的 off-by-one 会一路带到线上。',
  cause: '溢出值的边界（内联上限 3000/3001、页满 4080、两页 8160）没有逐个测试。',
  rule: '**每个格式边界一条测试**；错误用 `RC` 返回而不是 `throw`，断言用常开的 `verify`。',
  fix: 'A2 测试覆盖 3000/3001/4080/8160 四个边界 ± 1。',
  verify: 'A2 边界用例全绿。',
  cost: '无。',
  lesson: '边界值表是格式文档的一部分。',
  rel: [11, 64],
  sides: ['旧测法', '新测法'],
  code: { kind: 'sketch', src: '按 §12 第 47 条复原（测试侧）',
    old: `- put(k, value(5000)); get(k);      // 只测一个"大"值`,
    new: `+ for (n : {2999, 3000, 3001, 4079, 4080, 4081, 8159, 8160, 8161}) { put(k, value(n)); assert(get == value(n)); }`
  },
  old: [
    { t: '只测 5000 B：跨页 off-by-one 没暴露。', kv: { '覆盖边界': '0/4 ✗' }, ok: false },
  ],
  new: [
    { t: '四个边界 ±1 全测。', kv: { '覆盖边界': '4/4 ✓' }, ok: true },
  ],
});

PIT({
  id: 48, round: '第七批（快照）', cat: '快照 / 判据方向', found: 's4 B 视图 2000/2000 错', tags: ['snap_min', 'snap_max', '方向'],
  sym: '较新快照**整段丢版本**：s4 的 B 视图 2000/2000 错。',
  cause: '"老快照还需要的版本"上界是**最年轻**的快照（视图看 `ver <= 快照版本`），判别式却写成了 `snap_min()`。',
  rule: '**回收判据用 `snap_max()`**：只有 `ver > snap_max` 的版本对所有快照都不可见。',
  fix: 'min → max。',
  verify: 's4 全绿；后续 P5 精细化。',
  cost: '无。',
  lesson: '写快照判据时先在纸上列出"哪个快照会看到这个版本"，再决定 min/max。',
  rel: [7, 69, 91],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @1013 `smax`；老代码按 §12 第 48 条复原',
    old: `- if (r->ver <= snap_min()) keep;       // 方向反了：较新的快照需要的版本被回收`,
    new: `! const u32 smax = nsnap_ ? snap_max() : 0;
  if (r->ver <= smax) keep;             // 对最年轻的快照仍可见 ⇒ 保留`
  },
  old: [
    { t: '快照 A@5、B@9；版本 7 的旧值。`7 <= snap_min()=5` 为假 ⇒ 回收。但 B 看得见 7。', kv: { 'B 视图': '2000/2000 错 ✗' }, ok: false },
  ],
  new: [
    { t: '`7 <= snap_max()=9` ⇒ 保留。', kv: { 'B 视图': '✓' }, ok: true },
  ],
});

PIT({
  id: 49, round: '第七批（快照）', cat: '回退 / 状态完整性', found: 'rollback 后墓碑遮蔽新数据', tags: ['clear_state', 'ranges_', 'pend_ranges_'],
  sym: '回退后"删掉的范围"继续遮蔽回退之后重新写进去的数据。',
  cause: '`clear_state()` 不清 `ranges_` / `pend_ranges_`。',
  rule: '**回退必须把范围墓碑一起回到当时**（和坑 33 的 `pending_free_` 同类）。',
  fix: '`clear_state()` 清两个容器；重放清单重建 `ranges_`。',
  verify: 'rollback + del_range 回归。',
  cost: '无。',
  lesson: '"状态"清单要有一份显式列表，每加一个容器就加一行。',
  rel: [33, 62],
  code: { kind: 'sketch', src: '按 §12 第 49 条复原',
    old: `  void clear_state() { cells_.clear(); runs_.clear(); free_.clear(); ... }   // ranges_ 漏了`,
    new: `  void clear_state() { cells_.clear(); runs_.clear(); free_.clear();
!     ranges_.clear(); pend_ranges_.clear(); ... }`
  },
  old: [
    { t: '回退后 `ranges_` 仍含 [10,90)@v9；新写 put(50)@v10 ✓ 可见；但回退点之前的旧墓碑本不该存在。', kv: { 'get(50)': '按情形错 ✗' }, ok: false },
  ],
  new: [
    { t: '回退清空并由清单重建 ⇒ 墓碑回到回退点的状态。', kv: { 'get(50)': '✓' }, ok: true },
  ],
});

PIT({
  id: 50, round: '第七批（快照）', cat: '空闲表 / 重复计数', found: 'forget_snapshots 后分配器发两张票', tags: ['free_[p0] = n', 'carve_free', 'coalesce', '无锁'],
  sym: '空闲页比实际多，分配器对同一页**发出两张票**。',
  cause: '`forget_snapshots()` 无锁且用 `free_[p0] = n` 覆盖插入，和现有空闲块重叠时重复计数。',
  rule: '**向空闲表归还页的唯一顺序**：跳过 `>= tail_` → `carve_free()` 从现有块里挖掉 → 插入 → `coalesce()`；持锁。',
  fix: '如右。',
  verify: '`ownership_check` 无 `owned twice`。',
  cost: '无。',
  lesson: '集合型结构的"插入"要先定义重叠语义。',
  rel: [18, 33],
  code: { kind: 'sketch', src: '按 §12 第 50 条复原',
    old: `- for (auto& d : deferred_) free_[d.p0] = d.n;      // 与现有块重叠 ⇒ 重复计数`,
    new: `  for (auto& d : deferred_) {
+     if (d.p0 >= tail_) continue;
+     carve_free(d.p0, d.n);                // 先从现有块里挖掉
      free_insert(d.p0, d.n);
  }
+ coalesce();`
  },
  old: [
    { t: 'free_ 有 [20,30)；归还 [25,28) 直接覆盖插入。', rows: [{ n: 'free_', items: ['a:[20,30):free', 'b:[25,28) 重叠:bad'] }], kv: { '空闲页数': '13（真 10）✗' }, ok: false },
  ],
  new: [
    { t: 'carve 后插入再 coalesce ⇒ [20,30)。', rows: [{ n: 'free_', items: ['a:[20,30):good'] }], kv: { '空闲页数': '10 ✓' }, ok: true },
  ],
});

PIT({
  id: 51, round: '第七批（链接管）', cat: '大值 / 链所有权', found: '链被转走', tags: ['take_set_', 'count_refs', '按 run 记'],
  sym: '"同一次压实里还活着的另一条 run"的链被转走 ⇒ 它的大值 NOTFOUND / 读到别人的数据。',
  cause: '接管链只看 `nsnap_ == 0` 就动手；链的所有权是"按 run"记的，不是"按页"。',
  rule: '**接管链之前必须确认源 run 这次一定死**：`take_set_`——`compact_cell` 里是 `count_refs(rid) <= 1`，`merge_run_set` 里是全部 src。',
  fix: '`take_set_` 门禁；后续 M1 放宽为"快照期也可接管进 take_set_ 的 run"。',
  verify: 'A2 压实回归；`ownership_check`。',
  cost: '无。',
  lesson: '所有权转移的前提是"旧主人确定消失"，这个判断要显式。',
  rel: [52, 53, 55],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @4822~4834 `take_set_`；老代码按 §12 第 51 条复原',
    old: `- bool can_take(RunPtr src) { return nsnap_ == 0; }     // 源 run 可能还被别的单元引用`,
    new: `  // 有快照时也可以：进 take_set_ 的源 run 一定 ver > snap_max（level_pass / compact_cell 保证）
! bool can_take(RunPtr src) { return src && take_set_.count(src->page0) != 0; }`
  },
  old: [
    { t: 'run S 被两个单元引用；单元 C1 压实接管了 S 的链。', rows: [{ n: 'S', items: ['s:S（C2 仍引用）:data', 'c:链 → 转给 P:bad'] }] },
    { t: 'C2 读 S 的大值 ⇒ 链已归 P，随后被 P 释放 / 覆盖。', kv: { 'get': 'NOTFOUND / 错值 ✗' }, ok: false },
  ],
  new: [
    { t: '`count_refs(S) = 2 > 1` ⇒ 不进 take_set_ ⇒ 物化而非接管。', rows: [{ n: 'S', items: ['s:S:data', 'c:链保留:good'] }], ok: true },
  ],
});

PIT({
  id: 52, round: '第七批（链接管）', cat: '大值 / 状态标志', found: 'compact_file 后 verify ovfl data', tags: ['chain_taken', 'ensure_ovfl_loc', 'loc_ready', '空表 ≠ 未建'],
  sym: '`compact_file()` 之后 `verify` 报 `ovfl data`（链被后续分配覆写）。',
  cause: '`chain_taken()` 把 `ovfl_loc` 清空，而 `ensure_ovfl_loc()` 的守卫是 `!ovfl_loc.empty()` ⇒ 摘完立刻重扫数据页，把刚交出去的链**又找回来**，接着当自己的还掉。',
  rule: '**"表空"≠"还没建表"**：用一次性标志 `Run::loc_ready`，空表也算建过。',
  fix: '如右。',
  verify: 'compact_file + verify 回归。',
  cost: '一个 bool。',
  lesson: '用容器为空当"未初始化"是经典陷阱，尤其当容器可以被合法清空时。',
  rel: [51, 53],
  code: { kind: 'half', src: '新代码 = `include/fcs.h` @370 `loc_ready`、@4839；老代码按 §12 第 52 条复原',
    old: `- void ensure_ovfl_loc(RunPtr r) { if (!r->ovfl_loc.empty()) return; scan_pages_for_desc(r); }   // 摘链后空表 ⇒ 重扫 ⇒ 找回已交出的链`,
    new: `  bool loc_ready = false;        // ovfl_loc 已经建立过（**空表也算数**）
  void ensure_ovfl_loc(RunPtr r) {
!     if (!r || r->novfl == 0 || r->loc_ready) return;
      // 注意判据是 loc_ready 而不是 "ovfl_loc 是否为空"：摘链会把表清空，用"空表"当"没建过"就会去扫数据页，
      // 把刚交出去的链**又找回来**，接着当自己的还掉`
  },
  old: [
    { t: '源 S 的链交给 P（`chain_taken` 清空 S.ovfl_loc）。', rows: [{ n: 'S.ovfl_loc', items: ['e:∅:free'] }, { n: '链', items: ['c:归 P:new'] }] },
    { t: '`ensure_ovfl_loc(S)`：表空 ⇒ 重扫数据页 ⇒ 又"找到"链 ⇒ S 退休时把 P 的链释放。', rows: [{ n: '链', items: ['c:被 S 释放 → 被覆写:bad'] }], kv: { 'verify': 'ovfl data ✗' }, ok: false },
  ],
  new: [
    { t: '`loc_ready = true` 且表空 ⇒ 不重扫 ⇒ 链始终归 P。', rows: [{ n: '链', items: ['c:归 P ✓:good'] }], ok: true },
  ],
});
