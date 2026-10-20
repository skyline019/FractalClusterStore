import React from 'react'
import { Defs, Label, Note, Arrow } from '../ui.jsx'

export const STEPS = [
  { t: 'get(k) 先看内存', d: 'memtable 里命中就直接返回（还没提交的写也能读到自己的读）。未命中才走盘。' },
  { t: '每个 run 一道 Bloom', d: '每个 run 带一张 16 bit/键、4 探位的 Bloom（64 位哈希，误判 0.24%）。99% 的 run 在这里被排除 —— 这是“get 与 run 数几乎无关”的原因。' },
  { t: '单元里按 (seq, rid) 新→旧扫段', d: '段身份是 (seq, page0)，cell->segs 是有序 map，rbegin() 就是最新段。命中即停：新→旧第一个命中就是正确答案（旧段不可能有更高的 seq）。' },
  { t: '页内：fence 二分 + 前缀还原', d: '每页记 first/last 键，先二分定位页（run 还有 16 条/块的键域摘要 sp 再砍一层），读完页先校验 CRC，再把 8-pfx 的键后缀补回完整键。' },
  { t: '命中 / 墓碑 / 未找到', d: 'len=0 是墓碑 ⇒ RC::NOTFOUND（删除是“写了一条删除”）。整条路径的 probes/get 实测 0.12~0.65。' },
  { t: '缓存与快照', d: '页缓存是直映的：page(pid) 把页拷进 cache_[pid % N] 再返回指针 —— 所以指针活不过下一次 page()。快照读按 run->ver 过滤，不走 Bloom 的“最新”。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">一次 get 的读放大</div>
      <div className="stat-grid">
        <div><b>0.12</b><span>probes/get（碎提交）</span></div>
        <div><b>0.65</b><span>probes/get（200k 压测）</span></div>
        <div><b>0.24%</b><span>Bloom 误判</span></div>
        <div><b>1 页</b><span>命中时的页读</span></div>
      </div>
      <div className="panel-title" style={{ marginTop: 14 }}>过滤三层</div>
      <ol className="ordered">
        <li>Run Bloom<span className="dim">（16 bit/键 · 4 探位）</span></li>
        <li>键域摘要 sp<span className="dim">（每 16 条一块）</span></li>
        <li>页 fence 二分<span className="dim">（first/last）</span></li>
      </ol>
      <Note tone="amber">Bloom 是保守过滤器：它只会“说可能”，不会漏。哈希必须用完整 64 位 —— 截成 32 位会让 may() 恒假（坑 #30，代价是 17×）。</Note>
      {step >= 5 && <Note tone="red">直映缓存：跨页遍历时必须先把当前页拷出来，否则手里的指针会变成别的页（坑 #29）。</Note>}
    </>
  )
}

const RUNS = [
  { seq: 1867, lvl: 0, may: true, note: 'Bloom: MAYBE' },
  { seq: 1858, lvl: 1, may: false, note: 'Bloom: NO' },
  { seq: 1849, lvl: 1, may: false, note: 'Bloom: NO' },
  { seq: 1840, lvl: 2, may: true, note: 'Bloom: MAYBE' },
]

export default function Scene({ step }) {
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={40} size={13}>get(1002) →</Label>
      <rect x={126} y={24} width={110} height={24} rx={6} fill="rgba(56,225,255,.12)" stroke="rgba(56,225,255,.5)" />
      <text x={181} y={41} textAnchor="middle" fontSize="11.5" fill="#7ff0ff" fontFamily="var(--mono)">key = 1002</text>

      {/* 0 memtable */}
      <g opacity={step >= 0 ? 1 : 0.3}>
        <rect x={30} y={64} width={940} height={54} rx={9}
          fill={step === 0 ? 'rgba(56,225,255,.12)' : 'rgba(148,163,184,.05)'}
          stroke={step === 0 ? '#7ff0ff' : 'rgba(148,163,184,.25)'} />
        <text x={48} y={96} fontSize="12" fill="#7ff0ff" fontFamily="var(--mono)">① memtable（未提交的写也可见）</text>
        <text x={430} y={96} fontSize="11.5" fill={step === 0 ? '#86efac' : '#8fa2c7'} fontFamily="var(--mono)">
          {step === 0 ? '命中 key 1002 → 直接返回（无 I/O）' : '未命中 → 继续走盘'}
        </text>
      </g>

      {/* 1 bloom */}
      <Label x={30} y={152} size={11.5}>② Bloom 过滤（16 bit/键 · 4 探位）</Label>
      {RUNS.map((r, i) => {
        const active = step === 1 && r.may
        const dim = step >= 1 && !r.may
        return (
          <g key={i} opacity={dim ? 0.35 : 1}>
            <rect x={30} y={166 + i * 44} width={300} height={34} rx={6}
              fill={active ? 'rgba(62,224,139,.12)' : 'rgba(148,163,184,.05)'}
              stroke={active ? 'rgba(62,224,139,.6)' : 'rgba(148,163,184,.25)'} />
            <text x={44} y={188 + i * 44} fontSize="10.5" fill="#c9d6f5" fontFamily="var(--mono)">
              seq={r.seq} lvl={r.lvl}
            </text>
            {/* bloom bits */}
            {Array.from({ length: 16 }).map((_, b) => {
              const set = (b * 7 + i * 3) % 11 === 0
              return <rect key={b} x={136 + b * 6.5} y={176 + i * 44} width={4.5} height={13} rx={1}
                fill={set ? 'rgba(62,224,139,.7)' : 'rgba(148,163,184,.15)'} />
            })}
            <text x={258} y={188 + i * 44} fontSize="9.5" fill={r.may ? '#86efac' : '#64748b'} fontFamily="var(--mono)">
              {r.may ? 'MAYBE' : 'NO'}
            </text>
          </g>
        )
      })}

      {/* 2 段 */}
      <Label x={380} y={152} size={11.5}>③ 段：(seq, rid) 新→旧</Label>
      <rect x={380} y={166} width={270} height={162} rx={8} fill="rgba(167,139,250,.05)" stroke="rgba(167,139,250,.3)" />
      {[1867, 1858, 1849, 1840].map((s, i) => (
        <g key={s} opacity={step >= 2 ? (i <= 1 ? 1 : 0.45) : 0.5}>
          <rect x={392} y={176 + i * 36} width={246} height={28} rx={5}
            fill={step >= 2 && i === 0 ? 'rgba(167,139,250,.22)' : 'rgba(148,163,184,.05)'}
            stroke={step >= 2 && i === 0 ? '#c4b5fd' : 'rgba(148,163,184,.22)'} />
          <text x={402} y={195 + i * 36} fontSize="10.5" fill="#c4b5fd" fontFamily="var(--mono)">
            seg ({s}, rid={10 + i})
          </text>
          <text x={626} y={195 + i * 36} textAnchor="end" fontSize="9.5" fill={step >= 2 && i === 0 ? '#86efac' : '#8fa2c7'} fontFamily="var(--mono)">
            {step >= 2 && i === 0 ? '← 命中即停' : i === 1 ? 'Bloom: NO' : ''}
          </text>
        </g>
      ))}

      {/* 3 页 */}
      <Label x={672} y={152} size={11.5}>④ 页：fence 二分 + CRC</Label>
      <rect x={672} y={166} width={298} height={162} rx={8} fill="rgba(56,225,255,.05)" stroke="rgba(56,225,255,.3)" />
      <text x={686} y={188} fontSize="10.5" fill="#7ff0ff" fontFamily="var(--mono)">fences: [1001 … 1099]</text>
      {[0, 1, 2, 3].map((i) => (
        <rect key={i} x={686 + i * 70} y={198} width={62} height={22} rx={4}
          fill={step >= 3 && i === 1 ? 'rgba(56,225,255,.25)' : 'rgba(148,163,184,.06)'}
          stroke={step >= 3 && i === 1 ? '#7ff0ff' : 'rgba(148,163,184,.25)'} />
      ))}
      <text x={686} y={238} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">二分：log2(页数) 次比较，不解整页</text>
      <text x={686} y={258} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">page + 12..used 的 CRC32C 必须先过</text>
      <text x={686} y={278} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">键 = 页首键高 6 B ‖ 2 B 后缀</text>
      <text x={686} y={300} fontSize="10.5" fill={step >= 3 ? '#86efac' : '#8fa2c7'} fontFamily="var(--mono)">
        {step >= 3 ? '→ 解出 (key 1002, len 8, value v3)' : '等待定位'}
      </text>

      {/* 4 结果 */}
      <g opacity={step >= 4 ? 1 : 0.25}>
        <rect x={30} y={352} width={940} height={56} rx={9} fill="rgba(62,224,139,.07)" stroke="rgba(62,224,139,.4)" />
        <text x={48} y={376} fontSize="12" fill="#86efac" fontFamily="var(--mono)">
          返回：key=1002, ver=v3, len=8 —— 命中路径只读了 1 页
        </text>
        <text x={48} y={396} fontSize="11" fill="#8fa2c7" fontFamily="var(--mono)">
          墓碑（len=0）⇒ RC::NOTFOUND；Bloom 误判只是多读一页，不会读错值。
        </text>
      </g>

      {/* 5 缓存 / 快照 */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={30} y={424} width={452} height={150} rx={9} fill="rgba(255,200,87,.05)" stroke="rgba(255,200,87,.3)" />
        <text x={48} y={448} fontSize="12" fill="#fde68a" fontFamily="var(--mono)">直映页缓存</text>
        <text x={48} y={468} fontSize="10.5" fill="#c9d6f5" fontFamily="var(--mono)">page(pid) → cache_[pid % N] 的缓冲</text>
        <text x={48} y={488} fontSize="10.5" fill="#c9d6f5">返回的是“缓冲首地址”，下一次 page() 会原地改写它。</text>
        <text x={48} y={508} fontSize="10.5" fill="#fda4af" fontFamily="var(--mono)">跨页遍历 ⇒ 先 memcpy 当前页</text>
        <text x={48} y={528} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">replay_manifest / exact_live_recs 都栽过（坑 #29）</text>
        <text x={48} y={550} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">失效：inval(pid) 在页被重写时调用</text>
      </g>

      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={500} y={424} width={470} height={150} rx={9} fill="rgba(167,139,250,.05)" stroke="rgba(167,139,250,.3)" />
        <text x={518} y={448} fontSize="12" fill="#c4b5fd" fontFamily="var(--mono)">快照 / 游标</text>
        <text x={518} y={468} fontSize="10.5" fill="#c9d6f5">快照按 run-&gt;ver 过滤可见性；有快照时暂停回收。</text>
        <text x={518} y={488} fontSize="10.5" fill="#c9d6f5">游标是 k 路归并：每个 run 一个 Cur，键小的先出。</text>
        <text x={518} y={508} fontSize="10.5" fill="#c9d6f5">select/scan 的顺序读靠 sp + fence，不用整 run 解压。</text>
        <text x={518} y={528} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">范围扫 100 行：0.56 µs（异步+整理档）</text>
        <text x={518} y={550} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">SQLite 同负载：1.7 µs</text>
      </g>

      {/* 命中路径上的小光点 */}
      <path id="rd" d="M 180 78 L 180 178 L 520 178 L 690 178" fill="none" stroke="none" />
      <circle r="3.5" fill="#7ff0ff" opacity={step >= 1 && step <= 3 ? 1 : 0}>
        <animateMotion dur="1.8s" repeatCount="indefinite" keyPoints="0;1" keyTimes="0;1" calcMode="linear">
          <mpath href="#rd" />
        </animateMotion>
      </circle>
    </g>
  )
}
