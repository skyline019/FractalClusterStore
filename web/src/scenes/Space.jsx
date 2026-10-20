import React from 'react'
import { Defs, Label, Note } from '../ui.jsx'

export const STEPS = [
  { t: '空闲表按地址索引', d: 'map<起始页, 页数>：低地址优先分配（搬家/收缩要的就是低地址），释放时只看后继一眼就能合并成更大的块。' },
  { t: 'pending_free：提交点前不复用', d: '释放的页先进 pending_free_，提交点落盘之后才 promote 进空闲表。否则崩溃后“完整旧态”已经被新内容污染。' },
  { t: 'deferred：快照挡着就等', d: '有打开的 MVCC 快照时，旧版本页还不能回收；deferred_ 记着，等 nsnap_ 归零再统一放。压实/合并在 nsnap_>0 时整体暂停。' },
  { t: 'compact_file：归一两次再搬家', d: '第一次归一后旧段还占着低位，合并出来的 run 只能落在高处；再归一次，低位全空，一条 run 就能整块搬到最前面。一次归一 used 停在 3×live，两次之后 used ≈ live + 4 页。' },
  { t: 'shrink：先 checkpoint 清单', d: '清单链的页当年随手分配、飘在高处，不先重写清单，数据搬完了高水位也降不下来。shrink 之后文件真的会变小（bench4：2.90 MB → 2.10 MB）。' },
  { t: '容量规则：7/8 给你，其余留维护', d: '不增长的批（全是覆盖/删除）可以透支预留区，保证“满了也删得掉”。设备上限记在 max_pages，truncate 只能缩不能长 —— 所以逻辑容量取上限、物理长度单独记。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">三种“还没归位”的页</div>
      <div className="code">pending_free_  // 提交点后才 promote
deferred_      // 有快照，等 nsnap_ 归零
free_          // map&lt;起始页, 页数&gt;</div>
      <div className="panel-title" style={{ marginTop: 14 }}>实测</div>
      <div className="stat-grid">
        <div><b>live+4</b><span>两次归一后的 used（页）</span></div>
        <div><b>2.10 MB</b><span>文件（SQLite 2.35 MB）</span></div>
        <div><b>41.9 B</b><span>每记录字节（SQLite 46.9）</span></div>
        <div><b>7/8</b><span>用户可用容量</span></div>
      </div>
      <Note tone="amber">truncate 只能减不能增：一旦按物理长度封顶，库总有一天会写到 NOSPACE 且永远写不进去（连删除都要落页）。</Note>
      {step >= 4 && <Note tone="green">bench4 异步+整理档：compact_file 之后文件 2.90 MB → 2.10 MB，收缩耗时 31.9 ms。</Note>}
    </>
  )
}

function PageStrip({ x, y, cells, labels = true }) {
  return (
    <g>
      {cells.map((c, i) => (
        <rect key={i} x={x + i * 27} y={y} width={24} height={26} rx={3}
          fill={c === 'used' ? 'rgba(56,225,255,.38)' : c === 'free' ? 'rgba(62,224,139,.18)' : c === 'move' ? 'rgba(255,200,87,.35)' : 'rgba(148,163,184,.06)'}
          stroke={c === 'used' ? 'rgba(56,225,255,.8)' : c === 'free' ? 'rgba(62,224,139,.6)' : c === 'move' ? 'rgba(255,200,87,.7)' : 'rgba(148,163,184,.2)'} />
      ))}
      {labels && <text x={x} y={y + 42} fontSize="9.5" fill="#8fa2c7" fontFamily="var(--mono)">低地址 →</text>}
    </g>
  )
}

export default function Scene({ step }) {
  const after = step >= 3
  const cellsBefore = ['used','used','free','free','used','free','free','free','used','used','free','free','used','free','free','free','used','used','free','free','used','free','free','free','used','used','free','free','used','free']
  const cellsAfter = ['used','used','used','used','used','used','used','used','used','used','used','used','used','free','free','free','free','free','free','free','free','free','free','free','free','free','free','free','free','free']
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={26} size={13}>盘上页的三种状态（青=活 · 绿=空闲 · 黄=搬家）</Label>

      <g opacity={step >= 0 ? 1 : 0.35}>
        <text x={30} y={58} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">整理前</text>
        <PageStrip x={100} y={44} cells={cellsBefore} labels={false} />
        <text x={100 + 30 * 27 + 8} y={62} fontSize="9.5" fill="#fda4af" fontFamily="var(--mono)">碎片</text>
        <g opacity={step >= 3 ? 1 : 0.4}>
          <text x={30} y={100} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">整理后</text>
          <PageStrip x={100} y={86} cells={cellsAfter} labels={false} />
          <line x1={100 + 13 * 27} y1={86} x2={100 + 13 * 27} y2={112} stroke="#86efac" strokeWidth="1.4" strokeDasharray="3 3" />
          <text x={100} y={126} fontSize="9.5" fill="#86efac" fontFamily="var(--mono)">↑ 高水位 = live + 4 页，其后整段可 truncate</text>
        </g>
        <text x={540} y={126} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          归一两次 + 搬家：活页全在低处，高水位回落
        </text>
      </g>

      {/* 空闲表 */}
      <g opacity={step >= 0 ? 1 : 0.3}>
        <Label x={30} y={164} size={12}>free_ = map&lt;起始页, 页数&gt;（按地址，低地址优先）</Label>
        {[['2', '2'], ['5', '3'], ['11', '2'], ['13', '3'], ['19', '2'], ['21', '3'], ['26', '2'], ['28', '2']].map((r, i) => (
          <g key={i}>
            <rect x={30 + i * 118} y={176} width={108} height={30} rx={4} fill="rgba(62,224,139,.07)" stroke="rgba(62,224,139,.35)" />
            <text x={84 + i * 118} y={196} textAnchor="middle" fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">[{r[0]}, +{r[1]})</text>
          </g>
        ))}
        <text x={30} y={226} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">释放时看后继一眼：相邻就合并 —— 同一个 map，分配与合并都 O(log n)</text>
      </g>

      {/* pending_free */}
      <g opacity={step >= 1 ? 1 : 0.25}>
        <rect x={30} y={248} width={452} height={104} rx={9} fill="rgba(255,200,87,.05)" stroke="rgba(255,200,87,.3)" />
        <text x={48} y={272} fontSize="12" fill="#fde68a" fontFamily="var(--mono)">pending_free_（异步写）</text>
        <text x={48} y={292} fontSize="10.5" fill="#c9d6f5">commit(false) 已把页交给 worker，清单还是旧状态。</text>
        <text x={48} y={312} fontSize="10.5" fill="#c9d6f5">此时若让页号被重新分配，worker 会把新内容写到</text>
        <text x={48} y={330} fontSize="10.5" fill="#c9d6f5">“还挂在旧清单上”的页 ⇒ 崩溃后旧态也坏。</text>
        <text x={48} y={346} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">提交点之后 promote_pending() 才真正可用</text>
      </g>

      {/* deferred */}
      <g opacity={step >= 2 ? 1 : 0.25}>
        <rect x={498} y={248} width={472} height={104} rx={9} fill="rgba(167,139,250,.05)" stroke="rgba(167,139,250,.3)" />
        <text x={516} y={272} fontSize="12" fill="#c4b5fd" fontFamily="var(--mono)">deferred_（MVCC 快照）</text>
        <text x={516} y={292} fontSize="10.5" fill="#c9d6f5">快照要看见旧版本 ⇒ 旧页不能立刻回收。</text>
        <text x={516} y={312} fontSize="10.5" fill="#c9d6f5">nsnap_ &gt; 0 时：压实 / 合并 / 整 run 回收全部暂停。</text>
        <text x={516} y={330} fontSize="10.5" fill="#c9d6f5">快照关闭时归零，deferred 里的页统一放回 free_。</text>
        <text x={516} y={346} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">回退/重开时这两张表也要一起还原（坑 #33）</text>
      </g>

      {/* 搬家 */}
      <g opacity={after ? 1 : 0.2}>
        <Label x={30} y={378} size={12}>compact_file：归一两次 → 搬家 → 收缩</Label>
        {[
          ['① 归一（第一次）', '旧段还占着低位，新 run 只能落在高处'],
          ['② 归一（第二次）', '低位全空 ⇒ 一条 run 整块搬到最前'],
          ['③ checkpoint', '重写清单，别让高处的清单页卡住 shrink'],
          ['④ shrink', 'truncate 尾部；文件立刻变小'],
        ].map((r, i) => (
          <g key={i}>
            <rect x={30 + i * 236} y={392} width={224} height={64} rx={8} fill="rgba(56,225,255,.06)" stroke="rgba(56,225,255,.3)" />
            <text x={46 + i * 236} y={414} fontSize="11" fill="#7ff0ff" fontFamily="var(--mono)">{r[0]}</text>
            <text x={46 + i * 236} y={436} fontSize="10" fill="#8fa2c7">{r[1]}</text>
            {i < 3 && <path d={`M ${30 + i * 236 + 226} 424 L ${30 + (i + 1) * 236 - 2} 424`} stroke="rgba(148,163,184,.45)" strokeWidth="1.4" markerEnd="url(#ah)" />}
          </g>
        ))}
        <text x={30} y={480} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          used：一次归一后 ≈ 3 × live；两次之后 ≈ live + 4 页。
        </text>
      </g>

      {/* 7/8 */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={30} y={500} width={940} height={80} rx={9} fill="rgba(62,224,139,.05)" stroke="rgba(62,224,139,.3)" />
        <text x={46} y={524} fontSize="11.5" fill="#86efac" fontFamily="var(--mono)">容量：user_lim = 7/8 × cap_，预留区只给维护用；但“不增长的批”可以透支</text>
        <text x={46} y={544} fontSize="10.5" fill="#c9d6f5">
          满库 → 要删 → 删除也要落页 → 还是满，这是死锁。判据：新字节 ≤ 被替换字节（flush_mem 里一次 probe_prev 求和）。
        </text>
        <text x={46} y={564} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          truncate 只能减不能增 ⇒ 逻辑容量取设备上限 max_pages，物理长度单独记（坑 #22）。
        </text>
      </g>
    </g>
  )
}
