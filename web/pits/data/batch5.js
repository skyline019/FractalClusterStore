// 第 5 批：坑 110–112 + R17/R18 扩展（ROUND15–18）
// 每坑 ≥6 步，含 tbl 示例数据表；数字来自 docs/ROUND15–18.md。

PIT({
  id: 110, round: 15, cat: '高水位 / 二步搬家', found: 'ROUND15 hw2 / g15', tags: ['hw2_plan', 'hw_move_run', 'hw_blockers', '一提交点一条'],
  title: '高水位回收：一口气搬完会把读者钉死 → 二步搬家',
  sym: '老路径 `relocate` 在一把 XLock 里搬完顶部链 ⇒ 读者停顿与合并同量级；快照钉住时整段失败或半截布局。',
  cause: '搬家 = 重写整条 run。单提交点做完所有 blocker，锁持有时间 = Σ 源页 IO。',
  rule: '**一提交点只搬一条**；规划与执行分离；快照钉住是墙（`hw_blockers`），不穿透。',
  fix: '`hw2_plan` 列出可搬 run；`hw_move_run` 每步一条 + 提交；窗外落地。',
  why: '后台线程整段搬仍会长时间占锁；切片才与 mode1/3 预算同构。',
  verify: 'g15：规划器 / 一提交点一条 / 窗外落地 / 钉住是墙。',
  cost: '步数 ↑、末态收敛变慢；换读者 p99。',
  lesson: '空间回收的单位必须是「有界提交点」，不是「整条维护意图」。',
  rel: [108, 99, 'r14-4'],
  analysis: {
    trace: '老：plan 列出 R7,R8,R9 → 锁内连续搬三条 → 读者 get 被挡整段墙钟。\n新：每步只搬 R7 → 提交 → 放锁 → 再搬 R8；钉住的 R9 记入 hw_blockers，本轮跳过。',
    numbers: 'ROUND15：二步搬家治合并后静态占用；合并时刻 2× 峰值仍是格式问题（交给 P2b）。',
    walk: '读者：维护窗口内仍可能撞到单条搬运行；写者：每步 = 提交点。重开：已提交的搬家不可丢。'
  },
  code: { kind: 'half', src: 'ROUND15 / include/fcs.h hw2_*',
    old: `- // 一把锁搬完所有 blocker
- for (auto& r : blockers) relocate_run(r);  // 持锁时间 = Σ IO`,
    new: `+ auto plan = hw2_plan(target_pct);
+ // 每步只搬 plan 下一条；钉住 ⇒ hw_blockers++，本步跳过
+ RC hw_move_run(plan.next());`
  },
  old: [
    { t: '初态：活页 9.0 MB，tail 用到 21.3 MB（2.4×）；顶部链 R7/R8/R9 挡 shrink。',
      tbl: [['run','页','角色','钉住'],['R7','420','顶部','否'],['R8','380','顶部','否'],['R9','200','顶部','是(snap)']],
      kv: { 'used/live': '2.4×', 'hw_blockers': 0 }, rows: [{ n: '文件高水位', items: ['a:活区:data','b:洞:hole','c:R7:lock','d:R8:lock','e:R9:lock'] }] },
    { t: '触发 hw 回收：老码一次锁内搬 R7+R8+R9。',
      tbl: [['步骤','持锁','搬'],['1','XLock','R7+R8+R9 连续']],
      lanes: [{ n: '写者', ev: [{ a: 0, b: 90, l: '搬三条', k: 'lock' }] }, { n: '读者', ev: [{ a: 0, b: 90, l: '等待', k: 'wait' }] }], now: 50 },
    { t: 'R9 被快照钉住：整段失败或留下半截布局。',
      tbl: [['run','结果'],['R7','已搬'],['R8','已搬'],['R9','失败/半截 ✗']],
      kv: { '读者 max': '≈整段墙钟 ✗' }, ok: false },
    { t: '中途读者 get 全部堵在 XLock。', tbl: [['指标','值'],['get max','数十 ms ✗'],['进度','不可抢占']], ok: false },
    { t: '放弃或重试：空间曲线锯齿，测量不稳定。', tbl: [['used','状态'],['21→18→21','抖动 ✗']], ok: false },
    { t: '结论：一口气搬 = 把空间问题变成延迟问题。', tbl: [['判定','FAIL ✗']], log: ['!hw: snap pinned mid-batch'], ok: false },
  ],
  new: [
    { t: '同左初态。', tbl: [['run','页','钉住'],['R7','420','否'],['R8','380','否'],['R9','200','是']], kv: { 'used/live': '2.4×' } },
    { t: '`hw2_plan`：可搬 [R7,R8]；R9 → blockers。', tbl: [['plan','动作'],['R7','move'],['R8','move'],['R9','skip+block']], kv: { 'hw_blockers': 1 } },
    { t: '步 1：只搬 R7 → 提交 → 放锁。', tbl: [['步','搬','提交'],['1','R7','是 ✓']],
      lanes: [{ n: '写者', ev: [{ a: 0, b: 35, l: 'R7', k: 'lock' }, { a: 35, b: 45, l: 'commit', k: 'io' }] }, { n: '读者', ev: [{ a: 0, b: 35, l: '等', k: 'wait' }, { a: 45, b: 100, l: '通行', k: 'ok' }] }], now: 40 },
    { t: '步 2：搬 R8；读者在两步之间通行。', tbl: [['步','搬'],['2','R8 ✓']], kv: { 'used/live': '↓' }, ok: true },
    { t: 'R9 仍钉住：本轮不碰，墙记入统计。', tbl: [['R9','skip'],['hw_blockers','1 ✓']], ok: true },
    { t: 'g15 绿：有界步进 + 钉住是墙。', tbl: [['判定','PASS ✓']], log: ['+g15 hw2 ok'], ok: true },
  ],
});

PIT({
  id: 111, round: 16, cat: 'P2b / 分片 TRIM', found: 'ROUND16 layout / g16', tags: ['M_RUN_TRIM', 'choose_part', 'page0 锚', '养洞'],
  title: '整段重写峰值 ~2× → P2b 分片 + RUN_TRIM 养洞',
  sym: '合并把源整段重写到新空间再释放 ⇒ 峰值接近 2×；洞来得太晚，插入期 L0 已吃掉。',
  cause: '产物必须完整可读才提交；不切分就只能「先写满再删源」。',
  rule: '**片 = 有界键区间产物**；封口后 `M_RUN_TRIM` 推进源 `first`/页起点，**第 0 页永留作身份锚**。',
  fix: '`choose_part` 按洞挑片；TRIM 清单项跨重开可重放。',
  why: '稀疏 run（P2c）是下一刀；P2b 先把「养洞」做实。',
  verify: 'g16：分片 / TRIM 重放 / abort+rollback / 墓碑；layout 峰值主题见 ROUND16。',
  cost: 'run 数 ↑（片）；家族同 seq 规则更复杂（坑 112）。',
  lesson: '峰值是分配时序问题，不是「再合并狠一点」。',
  rel: [112, 'r17-2', 'r18-2'],
  analysis: {
    trace: '老：源 8 条 × 满写新空间 → peak≈2× → 再释放。\n新：片 1 写入洞 → TRIM 源前缀 → 片 2 再写；锚页 page0 保留。',
    numbers: 'ROUND16：合并前脏源已占 1.3~1.5×；分片复用要「先养洞」。',
    walk: '重开必须重放 TRIM；快照期 TRIM 作废规则见 g16。'
  },
  code: { kind: 'half', src: 'ROUND16 M_RUN_TRIM',
    old: `- // 整组合并：先完整写出产物，再删源
- write_full_output(srcs); free_srcs();`,
    new: `+ choose_part(hole); write_part();
+ // M_RUN_TRIM: page0=rid, npages=新起始偏移；第 0 页锚保留
+ append_man(M_RUN_TRIM, ...);`
  },
  old: [
    { t: '源家族 8 run，合计 800 页；最大洞 120 页。', tbl: [['源页','洞','peak 预估'],['800','120','~2×']], rows: [{ n: '盘面', items: ['s:源×8:data','h:洞120:hole','t:尾:free'] }] },
    { t: '老合并：在尾部写满 800 页产物。', tbl: [['used','800→1600'],['动作','整写']], kv: { 'peak': '2.0× ✗' } },
    { t: '提交后才释放源 ⇒ 峰值窗口已过。', tbl: [['释放','迟'],['L0','已吃洞']], ok: false },
    { t: '插入期再次把新洞填满。', tbl: [['洞','被 L0 吃 ✗']], ok: false },
    { t: 'layout 200k 峰停在高位。', tbl: [['peak','~1.9× ✗']], ok: false },
    { t: 'FAIL：没有养洞协议。', tbl: [['判定','FAIL ✗']], log: ['!peak stays high'], ok: false },
  ],
  new: [
    { t: '同左：800 页源，洞 120。', tbl: [['源','800'],['洞','120']] },
    { t: '`choose_part`：片长 ≤120，写入洞内。', tbl: [['part','110 页'],['落点','洞 ✓']], rows: [{ n: '盘面', items: ['s:源:data','p:片1:new','t:尾:free'] }], ok: true },
    { t: '`M_RUN_TRIM`：源 first 前移，page0 锚保留。', tbl: [['TRIM','+'],['锚 page0','保留 ✓']], ok: true },
    { t: '片 2 继续吃洞；峰值远低于 2×。', tbl: [['peak','↓'],['runs','+片']], ok: true },
    { t: '重开重放 TRIM：键域与页区间一致。', tbl: [['replay','OK ✓']], ok: true },
    { t: 'g16 PASS。', tbl: [['判定','PASS ✓']], log: ['+g16 trim ok'], ok: true },
  ],
});

PIT({
  id: 112, round: 16, cat: '墓碑 / 家族同 seq', found: 'ROUND16 §2.4 坑 1 / g16 tomb', tags: ['older_may_contain_grp', '同 seq 片', '复活'],
  title: '同 seq 家族片打破「seq 连续 ⇒ 更老」⇒ 墓碑被丢掉、旧值复活',
  sym: '键 52000/52002/52004 在修前复活；墓碑 run 与裁过的源同组。',
  cause: '老 `older_may_contain` 假定组内 seq 连续 ⇒ `SegKey < minkey` 即更老；同 seq 片打破假设。',
  rule: '**按「seq≤maxseq 且非本组源」的全部 run 探测**（键域+bloom），不依赖组内序。',
  fix: '`older_may_contain_grp(k, maxseq)`。',
  why: '改 seq 分配更伤格式；改探测口径局部。',
  verify: '实验头复活 3 键；正式头 0；g16 tomb。',
  cost: '探测面略增（仍 bloom）。',
  lesson: '不变量写进注释不够，测试要用「判别力」场景。',
  rel: [94, 96, 111],
  analysis: {
    trace: '墓碑盖 52000；裁过的源同 seq 仍含旧值；老规则判「下面无副本」丢墓碑 ⇒ get 见旧值。',
    numbers: '52000/52002/52004 三键；实验头 3 复活 / 正式头 0。',
    walk: '读路径与退休必须共用 grp 口径。'
  },
  code: { kind: 'half', src: 'ROUND16 older_may_contain_grp',
    old: `- // 假定组内 seq 连续
- if (segkey < minkey) return false; // 误判：同 seq 片`,
    new: `+ // 按 seq≤maxseq 且非本组源的所有 run（键域+bloom）
+ return older_may_contain_grp(k, maxseq);`
  },
  old: [
    { t: '家族：源 S trim 后 first=52010；墓碑 T 盖 52000..52004；同 seq。',
      tbl: [['对象','seq','键域'],['S','9','[52010,52100)'],['T','9','tomb 52000-04']],
      rows: [{ n: '组', items: ['S:源:data','T:墓碑:tomb'] }] },
    { t: '受害键 52000 只在更老 run 有副本。', tbl: [['键','更老副本'],['52000','有'],['52002','有'],['52004','有']] },
    { t: '老规则：同组 seq 连续 ⇒ 认「下面无副本」⇒ 丢 T。', tbl: [['T','丢弃 ✗']], ok: false },
    { t: 'get(52000) 读到旧值。', tbl: [['get','旧值复活 ✗']], log: ['!resurrect 52000'], ok: false },
    { t: '52002/52004 同样复活。', tbl: [['复活数','3 ✗']], ok: false },
    { t: 'FAIL。', tbl: [['判定','FAIL ✗']], ok: false },
  ],
  new: [
    { t: '同左。', tbl: [['S','trim'],['T','tomb']] },
    { t: '`older_may_contain_grp`：看见更老 run 仍可能含键。', tbl: [['探测','grp ✓']], ok: true },
    { t: '保留 T；get 被墓碑盖住。', tbl: [['get(52000)','NOTFOUND ✓']], ok: true },
    { t: '三键均不复活。', tbl: [['复活','0 ✓']], ok: true },
    { t: '实验头对照：3 vs 0。', tbl: [['判别力','有 ✓']], ok: true },
    { t: 'g16 tomb PASS。', tbl: [['判定','PASS ✓']], log: ['+tomb ok'], ok: true },
  ],
});

PIT({
  id: 'r17-1', round: 17, cat: '提交 / fsync 出锁', found: 'ROUND17 §1 / g17 / fsync_block', tags: ['commit_phase1/2/3', 'durable_vers', 'cmu_', 'sb_synced_'],
  title: '锁内两次 fsync 挡读者 79 ms → 阶段二出锁后 0.01 ms',
  sym: '慢 sync 设备上，提交者持 XLock 做两次 sync 时，读者 get 被钉在 ~79 ms（2×40 ms）。',
  cause: '可见性本就不依赖「已持久」；却把持久化 IO 放在排他锁内。',
  rule: '**锁内：flush_mem + 抓 SB 载荷；锁外：按票号 sync+写 SB**；页复用闸比 `sb_synced_`。',
  fix: '`commit_phase1/2/3` + 提交顺序器；`durable_vers()`。',
  why: '组提交/双 memtable 按拍板不做；先把 fsync 挪出锁。',
  verify: 'fsync_block 79→0.01 ms；stall p99.9 改善；g17；TSan。',
  cost: '阶段二失败窗口变长（dirty_ 重建）；锁内仍有 flush_mem。',
  lesson: '先量清「锁内到底在等什么」。',
  rel: ['r14-2', 99, 104],
  analysis: {
    trace: '老：lock → flush → sync → SB → sync → unlock。\n新：lock → flush+抓票 → unlock → 顺序器 sync/SB/sync → lock → 发布。',
    numbers: '睡盘 40 ms×2：get 79.26→0.01 ms；本沙箱 fsync p50 0.25 ms 时收益小。',
    walk: 'durable_vers 是跨线程持久自检；last_commit 裸读不可比。'
  },
  code: { kind: 'half', src: 'ROUND17 commit_phase*',
    old: `- xl_.lock();
- flush_mem(); sync(); write_sb(); sync();
- xl_.unlock();`,
    new: `+ phase1();                 // 锁内
+ phase2_ordered(cseq_);    // 锁外 sync+SB
+ phase3();                 // 锁内发布 sb_synced_`
  },
  old: [
    { t: '写者开始 commit；sync 被注入睡 40 ms。', tbl: [['sync 睡','40ms×2']], lanes: [{ n: '写者', ev: [{ a: 0, b: 100, l: '锁内双 sync', k: 'lock' }] }, { n: '读者', ev: [{ a: 10, b: 100, l: 'get 等待', k: 'wait' }] }], now: 50 },
    { t: '读者 get 开始。', tbl: [['get','阻塞']], kv: { '持锁': '是' } },
    { t: '仍卡在第一次 sync。', tbl: [['已等','~40ms']], ok: false },
    { t: '第二次 sync。', tbl: [['已等','~80ms ✗']], ok: false },
    { t: '测得 avg 79.26 / max 79.31 ms。', tbl: [['get avg','79.26ms ✗']], ok: false },
    { t: 'FAIL：持久化 IO 绑架读者。', tbl: [['判定','FAIL ✗']], log: ['!fsync_block 79ms'], ok: false },
  ],
  new: [
    { t: '同负载；phase1 锁内无等待。', tbl: [['phase1','flush+票 ✓']], lanes: [{ n: '写者', ev: [{ a: 0, b: 15, l: 'phase1', k: 'lock' }, { a: 15, b: 85, l: 'phase2 sync', k: 'io' }, { a: 85, b: 95, l: 'phase3', k: 'lock' }] }, { n: '读者', ev: [{ a: 0, b: 15, l: '等', k: 'wait' }, { a: 15, b: 85, l: 'get 通行', k: 'ok' }] }], now: 40 },
    { t: 'phase2 在锁外跑双 sync。', tbl: [['读者','可 get ✓']], ok: true },
    { t: '窗口内 get max 0.01~0.04 ms。', tbl: [['get avg','0.01ms ✓']], ok: true },
    { t: '`sb_synced_` 推进后才放行页复用。', tbl: [['闸','synced ✓']], ok: true },
    { t: 'g17 t_lockfree_sync / TSan PASS。', tbl: [['TSan','PASS']], ok: true },
    { t: 'PASS。', tbl: [['判定','PASS ✓']], log: ['+fsync out of lock'], ok: true },
  ],
});

PIT({
  id: 'r17-2', round: 17, cat: '分配 / hole_fit', found: 'ROUND17 §2.4 layout', tags: ['set_part_hole_fit', '中等产物', '<1024 页', '落尾'],
  title: '中等产物（<1024 页）不分片落尾 ⇒ 峰值 1.96×；hole_fit → 1.75~1.81×',
  sym: '大合并刚结束时落尾的中等产物把唯一好洞错过，峰值抬升。',
  cause: '旧逻辑只对「大产物」按洞分片；中等条直接 append 尾部。',
  rule: '**能进洞的中等产物按洞切**（默认开）。',
  fix: '`set_part_hole_fit(true)` / Soft 部署打开。',
  why: '时机旋钮（悬崖/脏度）实测更差，见 ROUND17 §2。',
  verify: 'layout 200k 1.96~2.00→1.75~1.81；800k 1.40→1.36；零写放大。',
  cost: '片数略增。',
  lesson: '先修分配，再拧合并时机。',
  rel: [111, 'r18-1', 'r18-2'],
  analysis: {
    trace: '合并释放大洞 → 中等产物 <1024 页 → 老：落尾；新：hole_fit 切进洞。',
    numbers: '200k −0.2× 量级；写字节不变。',
    walk: '与护洞正交：hole_fit 用洞，护洞防 L0 抢洞。'
  },
  code: { kind: 'half', src: 'ROUND17 hole_fit',
    old: `- if (part_pages >= 1024) choose_part(hole);
- else append_tail();`,
    new: `+ if (part_hole_fit_ || part_pages >= 1024) choose_part(hole);
+ else append_tail();`
  },
  old: [
    { t: '合并后洞 900 页；中等产物 600 页。', tbl: [['洞','900'],['产物','600']] },
    { t: '老：600<1024 ⇒ 落尾。', tbl: [['落点','尾 ✗']], rows: [{ n: '盘面', items: ['h:洞:hole','t:产物:new'] }], ok: false },
    { t: '洞留给随后 L0 插入。', tbl: [['洞','被 L0 吃']], ok: false },
    { t: '峰值 1.96~2.00×。', tbl: [['peak','1.96× ✗']], ok: false },
    { t: '写放大不降（只是更碎的布局）。', tbl: [['写','同']], ok: false },
    { t: 'FAIL。', tbl: [['判定','FAIL ✗']], ok: false },
  ],
  new: [
    { t: '同左。', tbl: [['洞','900'],['产物','600']] },
    { t: 'hole_fit：按洞切 600 页写入。', tbl: [['落点','洞 ✓']], ok: true },
    { t: '尾部不再无故拉高。', tbl: [['tail','稳']], ok: true },
    { t: '200k peak 1.75~1.81×。', tbl: [['peak','1.78× ✓']], ok: true },
    { t: '写字节不变。', tbl: [['写','齐平 ✓']], ok: true },
    { t: 'PASS。', tbl: [['判定','PASS ✓']], log: ['+hole_fit'], ok: true },
  ],
});

PIT({
  id: 'r17-3', round: 17, cat: '归并 / 家族游标', found: 'ROUND17 §3 famw / g17 t_wide_family', tags: ['open_chain', 'next_run', '源>64', 'CORRUPT 851'],
  title: '源 >64 时掩码溢出 live 漂移 → 家族虚拟源游标',
  sym: 'famw 400k/16/8：老头 verify CORRUPT 851 处；游标数=源数。',
  cause: '归并源下标掩码/上限按 64；P2b 片化后同源家族可 >>64。',
  rule: '**一家族一游标**：同 seq 且键域不相交的片串成链顺序读。',
  fix: '`Cur::open_chain/next_run`；源下标不限 64。',
  why: '加宽掩码只治表象；家族才是逻辑源。',
  verify: 'CORRUPT 851→0；游标 151→8；耗时 −2~7%。',
  cost: '默认 layout 家族不互并时量不到收益。',
  lesson: '正确性 bug 比性能故事优先。',
  rel: [111, 112],
  analysis: {
    trace: '151 片同家族 → 老：151 游标 + 掩码炸；新：1 链 8 家族游标。',
    numbers: '851 CORRUPT；151→8 游标。',
    walk: 'klo=max(slo,first) 因 trim 前缀。'
  },
  code: { kind: 'half', src: 'ROUND17 Cur::open_chain',
    old: `- u64 mask = 1ull << src_index; // src_index≥64 UB`,
    new: `+ cur.open_chain(s, &family_runs, lo, hi, hi_key);
+ // 片耗尽 → next_run()`
  },
  old: [
    { t: '家族 151 片，键域不相交、同 seq。', tbl: [['片','151'],['游标','151']] },
    { t: '源下标 64+ 触发掩码问题。', tbl: [['index','64+ ✗']], ok: false },
    { t: 'live 账漂移。', tbl: [['verify','CORRUPT']], ok: false },
    { t: '851 处错误。', tbl: [['errors','851 ✗']], log: ['!verify 851'], ok: false },
    { t: '归并宽度爆炸。', tbl: [['CPU','↑']], ok: false },
    { t: 'FAIL。', tbl: [['判定','FAIL ✗']], ok: false },
  ],
  new: [
    { t: '同左 151 片。', tbl: [['片','151']] },
    { t: '串成家族链，游标=家族数。', tbl: [['游标','8 ✓']], ok: true },
    { t: 'verify OK。', tbl: [['CORRUPT','0 ✓']], ok: true },
    { t: '耗时略降。', tbl: [['时间','−2~7%']], ok: true },
    { t: 'g17 t_wide_family PASS。', tbl: [['测试','PASS']], ok: true },
    { t: 'PASS。', tbl: [['判定','PASS ✓']], log: ['+family cursor'], ok: true },
  ],
});

PIT({
  id: 'r18-1', round: 18, cat: '分配 / 护洞', found: 'ROUND18 §2 layout_ab', tags: ['protect_merge_holes', 'skips', 'Soft'],
  title: '护洞：L0 跳过中部大洞；200k 峰值几乎不动，机制仍保留',
  sym: '直觉「L0 吃掉合并大洞」抬峰；护洞后 skips>0 但 200k peak 仍 ~1.8×。',
  cause: '峰主因是养洞长尾 + 中等产物；不是单洞被 L0 吃掉。',
  rule: '用户 `alloc_pages` 跳过 ≥64 页中部大洞；维护/流式仍可进洞；NOSPACE 回退。',
  fix: '`set_protect_merge_holes(true)`；Soft/`configure_deploy` 默认开。',
  why: '零写放大、大库略益（800k −0.04×）；不当 200k KPI。',
  verify: 'g18 t_protect；layout_ab/det。',
  cost: 'L0 更常长尾，默认预算下或略抬峰。',
  lesson: '机制正确 ≠ KPI 移动；要用 A/B 证伪叙事。',
  rel: ['r17-2', 'r18-2'],
  analysis: {
    trace: 'protect=1：用户写跳大洞（skips=15~60）；合并仍吃洞。200k det 1.82→1.81。',
    numbers: 'skips 有计数；写字节齐平；800k −0.04×。',
    walk: 'Soft 常驻打开；峰值下一刀仍是 P2c/养洞合并。'
  },
  code: { kind: 'half', src: 'ROUND18 protect',
    old: `- // alloc 直接 first-fit，可吃合并大洞`,
    new: `+ if (protect_holes_ && !maint && hole >= hole_protect_pages)
+   skip_hole();  // NOSPACE 时回退`
  },
  old: [
    { t: '合并释放 200 页中部洞。', tbl: [['洞','200'],['位置','中部']] },
    { t: 'L0 flush 50 页 first-fit 吃洞。', tbl: [['L0','吃洞 ✗']], ok: false },
    { t: '随后合并找不到大洞。', tbl: [['合并','落尾']], ok: false },
    { t: '叙事：这就是峰值元凶。', tbl: [['叙事','未验证']], ok: false },
    { t: 'A/B 前无法证伪。', tbl: [['证据','缺']], ok: false },
    { t: '（对照用）', tbl: [['—','—']], ok: false },
  ],
  new: [
    { t: '护洞开：L0 跳过 200 页洞。', tbl: [['skips','+'],['洞','保留 ✓']], ok: true },
    { t: 'det200k peak 1.81 vs 1.82。', tbl: [['Δpeak','−0.01']], ok: true },
    { t: '写字节不变。', tbl: [['写','齐平']], ok: true },
    { t: '800k −0.04× 弱益。', tbl: [['800k','1.49']], ok: true },
    { t: '不当 200k KPI；Soft 仍开。', tbl: [['产品','Soft 默认']], ok: true },
    { t: 'g18 t_protect PASS。', tbl: [['判定','PASS ✓']], log: ['+protect'], ok: true },
  ],
});

PIT({
  id: 'r18-2', round: 18, cat: '养洞 / 碎片切片 A', found: 'ROUND18 §5 layout_frag', tags: ['part_frag', 'frag_floor', 'choose_part', '−0.11×'],
  title: '碎片感知切片 A：det200k 1.82× → 1.71×（零写放大）',
  sym: '最大洞 < part_lo 但空闲总量够 ⇒ 老逻辑不肯降片，继续落尾抬峰。',
  cause: '片下限 part_lo 与洞尺寸错配。',
  rule: '若最大洞 < part_cap(part_lo) 且 free≥part_lo 且洞 ≥ frag_floor(16) ⇒ 在 [floor, part_lo) 按洞挑片。',
  fix: '`set_part_frag(true)`（默认 FCS_PART_FRAG=1）；计数 `part_frags()`。',
  why: '比再拧护洞更贴「养洞」真相。',
  verify: 'frag0→1：1.82→1.71，part_frags=2；g16/17/18 绿。',
  cost: 'run +9（200k）。',
  lesson: '峰值刀要砍在 choose_part。',
  rel: [111, 'r18-1', 'r18-3'],
  analysis: {
    trace: '洞 40、part_lo 80 → 老落尾；新降到 40 片写入洞，frags++。',
    numbers: '−0.11×；写 41.8 MB 齐平。',
    walk: 'Soft/configure_deploy 打开 frag A。'
  },
  code: { kind: 'half', src: 'ROUND18 choose_part frag',
    old: `- if (largest_hole < part_lo) append_tail();`,
    new: `+ if (frag_ && largest_hole < part_lo && free >= part_lo && largest_hole >= frag_floor)
+   pick_part_in(frag_floor, part_lo);  // part_frags++`
  },
  old: [
    { t: 'largest_hole=40，part_lo=80，free=200。', tbl: [['洞','40'],['part_lo','80'],['free','200']] },
    { t: '老：洞 < part_lo ⇒ 落尾。', tbl: [['落点','尾 ✗']], ok: false },
    { t: '峰值停留 1.82×。', tbl: [['peak','1.82× ✗']], ok: false },
    { t: 'frags=0。', tbl: [['part_frags','0']], ok: false },
    { t: '写量 41.8 MB。', tbl: [['写','41.8']], ok: false },
    { t: 'FAIL（相对可达成的 1.71）。', tbl: [['判定','偏高 ✗']], ok: false },
  ],
  new: [
    { t: '同左。', tbl: [['洞','40'],['part_lo','80']] },
    { t: '降片下限到 40，写入洞。', tbl: [['part','40 ✓'],['落点','洞']], ok: true },
    { t: 'part_frags=2（det200k）。', tbl: [['frags','2 ✓']], ok: true },
    { t: 'peak 1.71×。', tbl: [['peak','1.71× ✓']], ok: true },
    { t: '写仍 41.8 MB。', tbl: [['写','齐平 ✓']], ok: true },
    { t: 'g18 t_frag PASS。', tbl: [['判定','PASS ✓']], log: ['+frag A'], ok: true },
  ],
});

PIT({
  id: 'r18-3', round: 18, cat: '养洞 / 并洞 B', found: 'ROUND18 §6 layout_coal', tags: ['part_coalesce', '搬挡路', '默认关'],
  title: '有界并洞 B：默认关——搬挡路把峰从 1.71 抬到 1.74',
  sym: '并洞本意连碎片；实测先搬挡路 run 的写放大抬峰。',
  cause: '挡路数据必须先搬走才能合并空洞。',
  rule: '**可选、默认关**；需要时 `set_part_coalesce(true)`。',
  fix: '保留代码与 g18 t_coal；Soft **不**启用 B。',
  why: 'A 已 −0.11×；B 负收益不进默认。',
  verify: 'coal 开：1.71→1.74；t_coal 行为正确。',
  cost: '额外搬移写。',
  lesson: '落地不等于默认开。',
  rel: ['r18-2'],
  analysis: {
    trace: '需要 117 页连续；挡路 97→搬到 3；coalesces=1；峰反升。',
    numbers: '1.71→1.74。',
    walk: '产品文档写明默认关。'
  },
  code: { kind: 'half', src: 'FCS_PART_COALESCE=0',
    old: `- // 无并洞`,
    new: `+ if (part_coalesce_) move_blocker_then_merge_holes(); // 默认 off`
  },
  old: [
    { t: '（无 B）峰 1.71。', tbl: [['peak','1.71'],['B','off']], ok: true },
    { t: '对照基线。', tbl: [['写','基线']], ok: true },
    { t: '—', tbl: [['—','—']] },
    { t: '—', tbl: [['—','—']] },
    { t: '—', tbl: [['—','—']] },
    { t: '基线 OK。', tbl: [['基线','1.71']], ok: true },
  ],
  new: [
    { t: '打开 B：需要 117 连续洞。', tbl: [['need','117']] },
    { t: '挡路 run 97 页要先搬。', tbl: [['blocker','97→3']], rows: [{ n: '搬', items: ['b:挡路:bad','h:洞:hole'] }] },
    { t: 'coalesces=1，洞合并。', tbl: [['coalesces','1']], ok: true },
    { t: '但峰 1.74×（变差）。', tbl: [['peak','1.74 ✗']], ok: false },
    { t: '产品决策：默认关。', tbl: [['默认','OFF ✓']], ok: true },
    { t: 'g18 t_coal 验证机制；Soft 不启用。', tbl: [['判定','机制 PASS / 默认 OFF']], log: ['+coal default off'], ok: true },
  ],
});

PIT({
  id: 'r18-4', round: 18, cat: '部署 / 整理态', found: 'ROUND18 / g18 t_tidy_sm', tags: ['TidyPolicy', 'configure_deploy', 'polish', 'maybe_tidy'],
  title: '整理状态机：Soft 常驻 / Hard 步进 / Periodic 重锤 / polish',
  sym: '旋钮散落；部署不知「日常」与「对外读服务前」用哪套。',
  cause: '缺一等公民策略对象。',
  rule: '**Soft**=读友好常驻；**Hard**=压力下 consolidate_step；**Periodic**=compact_file_step；**polish**=consolidate+shrink。',
  fix: '`set_tidy_policy` / `tidy_snap` / `maybe_tidy` / `configure_deploy`。',
  why: '比让应用自己拼 async+cheap+hole_fit+protect 更不容易漏。',
  verify: 'g18 t_tidy_sm：runs/layout%/protect 快照。',
  cost: 'Hard/Periodic 写放大↑，需显式。',
  lesson: '部署 API 要小、默认要安全。',
  rel: ['r18-1', 'r18-2', 'r17-2'],
  analysis: {
    trace: 'Soft→configure_deploy；压力→Hard 步进；发布前 polish 或 Periodic 循环。',
    numbers: 'tidy_snap：policy/runs/used/live/layout_pct/protect。',
    walk: '组提交仍由应用拼；整理态不替代业务批量。'
  },
  code: { kind: 'half', src: 'configure_deploy / polish',
    old: `- // 应用自己拼 set_async + cheap + ...`,
    new: `+ configure_deploy();           // Soft
+ set_tidy_policy(Hard);
+ while (maybe_tidy()==MORE) ;`
  },
  old: [
    { t: '应用漏开 hole_fit 或护洞。', tbl: [['配置','不全 ✗']], ok: false },
    { t: '日常与发布前用同一套重锤。', tbl: [['写放大','意外 ↑']], ok: false },
    { t: '无快照可观测。', tbl: [['snap','无']], ok: false },
    { t: '—', tbl: [['—','—']] },
    { t: '—', tbl: [['—','—']] },
    { t: 'FAIL：部署契约不清。', tbl: [['判定','FAIL ✗']], ok: false },
  ],
  new: [
    { t: '`configure_deploy`：async+cheap+hole_fit+frag+protect+hw。', tbl: [['Soft','开 ✓']], ok: true },
    { t: '`tidy_snap` 可读 runs/layout%/protect。', tbl: [['protect','1'],['layout','109%']], ok: true },
    { t: 'Hard：`maybe_tidy` → consolidate_step。', tbl: [['Hard','步进 ✓']], ok: true },
    { t: '发布前 `polish()` 归一+shrink。', tbl: [['polish','重锤']], ok: true },
    { t: 'B 保持默认关。', tbl: [['coalesce','OFF']], ok: true },
    { t: 'g18 t_tidy_sm PASS。', tbl: [['判定','PASS ✓']], log: ['+tidy sm'], ok: true },
  ],
});
