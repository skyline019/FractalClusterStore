// 全部数字取自仓库 docs/DESIGN3.md §9/§10/§11 与 tests、bench 的最近一次实测（2026-10-19）。

export const CONSTS = [
  { k: 'PAGE', v: '4096 B', note: '物理页大小；页头 + 记录 + 空闲' },
  { k: 'DHDR', v: '40 B', note: 'kind / next / chk / ver / first / last / nrec / used / pfx / fmt' },
  { k: 'REC_HDR', v: '10 B', note: 'key(8) + len(2)，2 B 对齐；len=0 即墓碑' },
  { k: 'BMAX', v: '32 KB', note: '单元字节上限，超过即零搬移切分' },
  { k: 'SMAX', v: '512', note: '单元段数安全阀' },
  { k: 'lvl_t / LVL_MAX', v: '8 / 4', note: '分层合并：每层 run 上限（运行时旋钮）/ 层数' },
  { k: 'async_flush_min', v: '2048', note: '异步组提交：攒多少条记录落一批页' },
  { k: 'io_threads', v: '1', note: '后台写线程数（set_io_threads）' },
  { k: 'FCS_GC_RATIO', v: '2', note: '压实性价比：dead ≥ 2 × live 才重写' },
  { k: 'FCS_LVL_DIRTY_PCT', v: '30%', note: '分层合并的垃圾门槛（本轮从 20% 提到 30%）' },
  { k: 'INLINE_MAX', v: '32', note: '超级块里内联的增量清单条数' },
  { k: 'MAN_SNAP_AT', v: '64', note: '清单链长度上限，超过写全量快照' },
  { k: 'Bloom', v: '16 bit/键 · 4 探位', note: '误判 ≈ 0.24%（64 位哈希）' },
  { k: 'SPARSE / sp', v: '16 条 / 16 条', note: '页内稀疏索引 / run 级键域摘要（纯 RAM）' },
  { k: 'kidx', v: '8 B/键', note: '可选精确键索引（set_key_index）' },
  { k: 'io_timeout_ms', v: '0（不限）', note: '有界等待；>0 时任何等待到期返回 RC::TIMEOUT' },
]

export const BENCH3 = [
  { ops: '20 000 / 500', wa: 2.53, probes: 0.12, seg: 9 },
  { ops: '20 000 / 100', wa: 3.44, probes: 0.12, seg: 14 },
  { ops: '50 000 / 1 000', wa: 2.21, probes: 0.27, seg: 19 },
  { ops: '200 000 / 2 000', wa: 2.57, probes: 0.65, seg: 7 },
]

export const BREAKDOWN = [
  { name: 'flush', bytes: 4.08, tone: 'cyan', note: '提交时 memtable 顺序落页' },
  { name: 'merge', bytes: 3.32, tone: 'violet', note: '分层合并搬活记录' },
  { name: 'manifest', bytes: 0.58, tone: 'amber', note: '每次提交的清单增量页' },
]
export const LIVE_BYTES = 3.10 // MB，逻辑写入量

export const BENCH4 = [
  { phase: '写入（含提交）', sync: 0.76, async: 0.58, sqlite: 2.1 },
  { phase: '覆盖写', sync: 1.30, async: 0.98, sqlite: 14.0 },
  { phase: '点查', sync: 0.59, async: 0.51, sqlite: 1.7 },
  { phase: '区间扫 100 行', sync: 1.89, async: 0.56, sqlite: 1.7 },
]

export const EVOLUTION = [
  { label: '修 Bloom 之前', sec: 16.29, reads: 205528, tone: 'red' },
  { label: '修 Bloom 之后', sec: 0.95, reads: 81245, tone: 'amber' },
  { label: '本轮（门槛 30% + 按需写）', sec: 0.36, reads: 50346, tone: 'green' },
]

export const PITS = [
  // ---------- 结构 ----------
  { id: 1, cat: '结构', title: '幽灵段', body: '切分后给子 cell 保留“该 run 在本区间没有记录”的段，日后合并回来会重复计入。', fix: '段必须带键域，且键域非空才保留。' },
  { id: 2, cat: '结构', title: 'cell 回并产生重复副本', body: 'cell 回并会把邻居的记录重新纳入视野，与压实段形成重复副本。', fix: 'cell 只细分，不回并。' },
  { id: 3, cat: '结构', title: '压实产物 seq 撞号', body: '压实产物的 seq 必须落在“栈底”且全局唯一，否则会和被它替换的 run 撞号，seg map 里一条覆盖另一条，读到的就是旧值。', fix: '段身份用 (cell, seq, rid) 全序；压实/合并产物取源的最大 seq。' },
  { id: 4, cat: '结构', title: '页号会复用', body: '清单里的 M_SEG_DEL 必须带 seq。组提交/分层合并引入后再次咬人：重放时删掉了刚复用同一页号的新 run。', fix: '段身份 = (cell, rid, seq)，删除项必须写全。' },
  { id: 9, cat: '结构', title: '合并产物要继承最新 seq', body: '分层合并的产物必须继承“源里最新的 seq”，否则旧值复活。', fix: 'nr->seq = max(src->seq)。' },
  { id: 16, cat: '结构', title: '胜者规则只能有一条', body: '可见版本最大者胜，同版本 seq 新者胜。按“段的先后”读会在“低 seq + 高版本”的合并产物上读到旧值。', fix: '读路径与归并统一用 (seq, rid) 全序。' },
  { id: 20, cat: '结构', title: '切分后没收紧页区间（最贵的一个）', body: 'split() 只改键域不动页区间（零搬移），但页区间不收窄，每个单元都要从 run 首页开始扫 —— 单元数 × 页数，四舍五入就是 O(n²)。', fix: '切分后 retighten()。实测 30k 全扫 698 → 68 ns/条（10×）；一次 50k 提交 195 → 22.6 ms（8.6×）。' },

  // ---------- 记账 ----------
  { id: 10, cat: '记账', title: '合并时别乱减源 run 的存活账', body: '分层合并时递减源 run 的存活账，会把还活着的 tombstone 当成垃圾回收掉。', fix: '源 run 随后整条删除，其账无需维护；新产物的账单独算。' },
  { id: 15, cat: '记账', title: '记账只能有一条算术', body: 'live/dead/pages 散在五六个地方做增量补丁，漏一处就是一次“live 漂移”：cell 该分裂不分裂、空间虚高、压实选错受害者，而且 verify 一开始查不出来（两边用同一份错账）。', fix: '统一由 recount(cell) 从段集合直接算。' },
  { id: 27, cat: '记账', title: '“脏不脏”要用精确账', body: 'run->live_bytes 是合并时用布隆算的上界（只会多算），拿它判断“值不值得重写”，结果垃圾越多越不肯重写，空间放大卡在 3×。', fix: '改用 Σ cell->live（精确、verify 每轮核对）。' },
  { id: 31, cat: '记账', title: '回收前必须精确复核', body: 'retire_dead_runs 判“整条 run 全死”时，不能只听增量计数。', fix: 'exact_live_recs() 扫一遍确认；计数只是提示值，复核堵死“误删活数据”。' },
  { id: 34, cat: '记账', title: '合并产物的存活账不能按自己的 seq 反推', body: '合并后的 run 只有一个 seq（= 源里最新的那个），拿它去问“这条记录有没有更新副本”，会把从旧源搬来的记录算成新的。实测热点负载一条 run 记 118 条活、真值 40。', fix: '用产物自己的段身份 (seq, page0) 判，且等 finish_run 分配完页号之后再判。' },
  { id: 35, cat: '记账', title: '切分时段计数要退回', body: '段级记账里 run->live_recs 是它在所有单元里的 live 之和。切分用 segs.swap() 换掉整张表，原 Seg 连同计数一起被丢掉，随后父子各 recount 一次 → 同一批记录加两遍。', fix: 'swap 前 seg_set_live(sg, 0)；现象是 live_recs 恰好是 nrec 的 2 倍。' },
  { id: 36, cat: '记账', title: '压实产物的 seq 要在删除源之前取', body: 'compact_cell 原来在释放源 run 之后才去 runs_ 里找它们的 seq/lvl，删掉的自然找不到，拿到的是一条无关 run 的 seq。', fix: '先记 best_seq/best_lvl，再删源；否则产物和活 run 撞号，胜负随手一翻就变。' },

  // ---------- 并发 / 异步 ----------
  { id: 5, cat: '异步', title: '提交点前不能复用刚释放的页', body: '否则崩溃后“完整旧态”已被破坏。', fix: 'pending_free_：提交点之后才 promote 进空闲表。' },
  { id: 14, cat: '异步', title: '异步在飞的页不能复用', body: 'commit(false) 已把页交给后台线程，但清单还是旧状态；此时 free_page 让它被重新分配，后台线程会把新内容写到“还挂在旧清单上”的页 → 崩溃后旧态也坏。', fix: '异步 flush 只做 flush；回收/合并留在提交点。' },
  { id: 25, cat: '异步', title: '在飞时重建状态 = 用旧页覆盖新页', body: 'rollback/reload 不 wait_io() 的话，worker 手里还握着旧分支的页，写下去正好落进刚回收重分配的位置，症状是随机坏清单项（npages 变成天文数字）。', fix: '任何“按盘重建内存”之前，先把在飞的 I/O 等干净。' },
  { id: 19, cat: '异步', title: '异步的收益在停顿分布，不在吞吐', body: '页写本来就是一批一次顺序写，前台 CPU 才是瓶颈。把 I/O 挪到后台换来工作区 8.4 MB → 2.8 MB 与更平缓的停顿，吞吐只能持平。', fix: '落盘粒度按字节而不是按提交：每 8192 条落一段会产生 24 条与全键域重叠的 run，实测慢 2.3×（4.6 → 10.7 µs/op）。' },

  // ---------- I/O 与设备 ----------
  { id: 6, cat: 'I/O', title: 'NOR 只能 1→0', body: '先擦后写，且擦除必须发生在提交点之后。', fix: '超级块的擦除走“提交点之后”的延迟队列。' },
  { id: 12, cat: 'I/O', title: '部分页写必须 4 B 对齐', body: 'RamDev::valid 要求 off%4==0 && len%4==0，否则整次提交返回 RC::IO。', fix: '清单页只写 MHDR + n*MENT，也是 4 B 对齐的。' },
  { id: 17, cat: 'I/O', title: '收缩会被清单卡住', body: '数据都搬到低处了，高水位还是下不来 —— 清单链的页是当年随手分配的，飘在高处。', fix: '先 checkpoint() 重写清单，shrink() 才有意义。' },
  { id: 21, cat: 'I/O', title: 'CRC 要走硬件', body: '查表 374 MB/s，一页 4 KB 是 11 µs；x86 crc32 指令 7.5 GB/s，一页 0.6 µs。每页一次校验，差 20 倍。', fix: 'SSE4.2 crc32c；软件回退保留。' },
  { id: 22, cat: 'I/O', title: 'truncate 只能减不能增 = 永久封顶', body: 'shrink() 之后设备再也长不回去，库会在某天写到 NOSPACE 之后永远写不进（连删都删不掉，因为删除也要落页）。', fix: 'max_pages 记设备上限，逻辑容量取上限、物理长度单独记，按需 truncate 增长。' },
  { id: 23, cat: 'I/O', title: '满库必须“删得掉”', body: '只给用户 7/8 容量还不够：一批全是覆盖/删除的写不增长存活集，必须允许它动用预留区，否则“满了 → 要删 → 删也要空间 → 还是满”死锁。', fix: '判据“新字节 ≤ 被替换字节”，flush_mem 里一次 probe_prev 求和。' },
  { id: 24, cat: 'I/O', title: '维护不能把提交搞失败', body: '压实/合并/清单页都是机会性的。旧实现里 merge_run_set 一次 NOSPACE 就把整个用户提交带崩，用户看到“库满了”，实际是垃圾没地方腾。', fix: '拿不到空间就跳过本轮，不回传错误。' },
  { id: 29, cat: 'I/O', title: '页缓存是直映的，页指针活不过一次 page()', body: 'page(pid) 把页拷进 cache_[pid % N] 并返回首地址。遍历一页时再读别的页，手里那个指针会原地变成另一页的内容。replay_manifest 就这么“读到一条来自数据页的记录”；exact_live_recs 也踩了同一个坑（存活数恒为 0）。', fix: '跨页操作必须先把当前页拷出来。' },
  { id: 8, cat: 'I/O', title: '压实只保留最新版本', body: '所以“前缀合并”永远回收不了空间，必须整 cell 重写。', fix: '单元级压实；前缀只用于页内编码。' },
  { id: 28, cat: 'I/O', title: 'compact_file 要归一两次', body: '第一次归一时旧段还占着低位，合并出来的 run 只能落在高处；收完再归一次，低位全空 ⇒ 一条 run 能整块搬到最前面。', fix: '一次归一时 used 停在 3×live；两次之后 used ≈ live + 4 页。' },

  // ---------- 恢复 / 持久 ----------
  { id: 7, cat: '恢复', title: '压实与快照互斥', body: '有快照时暂停回收，否则快照视图看不到旧值。', fix: 'nsnap_ > 0 时 compact/merge/retire 全部跳过。' },
  { id: 13, cat: '恢复', title: '游标跨版本要按 run 的 ver 过滤', body: '记录版本已经不在记录头里了（头只有 10 B）。', fix: '游标按 run->ver 过滤可见性。' },
  { id: 26, cat: '恢复', title: '清单项必须自证清白', body: 'apply_man 里照着 m.npages 分配，就是给了坏数据一个申请 9 GB 内存的机会。', fix: '越界条目一律丢弃 + 计数，挂载永远不因一条坏记录失败。' },
  { id: 32, cat: '恢复', title: '回退必须让逻辑时钟继续前进', body: 'rollback 把 vers_ 恢复到快照点，另一个超级块槽里“版本更高的旧分支”就会在下次挂载时胜出 —— 回退被静默复活。', fix: 'vers_ = 回退前的时钟 + 1：回退的是状态，不是历史。' },
  { id: 33, cat: '恢复', title: '回退要一起还原回收账', body: 'pending_free_/deferred_ 里是“还没归位”的页。不还原，回退后它们会被 promote_pending() 放进空闲表 —— 而那些页属于被回退掉的分支，于是同一页被分配两次。', fix: '快照把它们整体存下来，回退时按内容还原。' },

  // ---------- 工程 ----------
  { id: 11, cat: '工程', title: '记录头改窄必须一次改全', body: '16 B → 10 B 时有 4 处 off + 16 <= used 漏改，表现为“提交后 get 不到”。', fix: '改完 grep \'16 <= used\' 与 \', 16)\'。' },
  { id: 18, cat: '工程', title: '空闲表要按地址索引', body: '原来是 multimap<长度, 起始页>（按大小最优适配），搬家/收缩恰恰要“最低地址”。', fix: '改成 map<起始页, 页数>：低地址优先、邻居合并只看一眼后继。' },
  { id: 30, cat: '工程', title: '64 位哈希被截成 32 位 ⇒ Bloom 恒假', body: 'Bloom::add 用完整 64 位哈希落 4 个位，查询侧写成 u32 hk，于是 may() 几乎总是 false。后果不是读错数据，而是存活账永远算满：零写回收从不触发、run 越堆越多、读页从 8 万涨到 20 万、同一负载 0.95 s → 16.29 s（17×）。', fix: '布隆是保守过滤器，错的方向只会让最优路径失效、不会报错 —— 存活账必须精确复核兜底。' },
]

export const CATS = ['全部', '结构', '记账', '异步', 'I/O', '恢复', '工程']

export const SCENES = [
  { id: 'overview', no: '00', title: '全景', sub: '一次 put 到盘上的七站' },
  { id: 'page', no: '01', title: '页的解剖', sub: '40 B 页头 · 10 B 记录头 · 前缀压缩' },
  { id: 'write', no: '02', title: '写入与提交', sub: 'memtable → run → 组提交 → 提交点' },
  { id: 'manifest', no: '03', title: '清单与超级块', sub: '双槽 · 内联增量 · 链式快照' },
  { id: 'read', no: '04', title: '读路径', sub: 'Bloom → 段 → fence → 页内解码' },
  { id: 'compact', no: '05', title: '合并与压实', sub: 'LSM 分层 · 垃圾门槛 · 写放大分解' },
  { id: 'space', no: '06', title: '空间回收', sub: 'pending_free · 快照延迟 · 搬家收缩' },
  { id: 'crash', no: '07', title: '崩溃恢复', sub: '掉电注入 · CRC · 双槽择优' },
  { id: 'hang', no: '08', title: '绝不挂死', sub: '有界等待 · 停滞快失败 · 自愈' },
  { id: 'pits', no: '09', title: '36 个坑', sub: '每条都变成了不变量' },
  { id: 'format', no: '10', title: '真字节', sub: '从设备里 dump 出来的页 / 超级块 / 清单' },
  { id: 'data', no: '11', title: '实测数据', sub: 'bench3 / bench4 / 19 项测试' },
]

export const STATS = [
  { k: '写放大', v: '2.57×', note: '200k/2000，去年 3.14×' },
  { k: '耗时', v: '0.36 s', note: '同一负载 45× 提升' },
  { k: '读页', v: '50 346', note: '段数峰值 7' },
  { k: '最坏等待', v: '150 ms', note: '3 s 卡顿设备下的提交上限' },
]
