import React from 'react'
import { Defs, Label, HBar, Note } from '../ui.jsx'

export const STEPS = [
  { t: '分层：每层最多 8 条 run', d: '新 flush 落在 L0；当某一层的 run 数达到 lvl_t（默认 8）时，把最旧的一组升到下一层。层数上限 4。目的只有一个：把 k 路归并的 k 压小，控制读放大。' },
  { t: '什么时候值得合并？看垃圾比例', d: '合并要重写数据。只有这一层的垃圾占比 ≥ FCS_LVL_DIRTY_PCT（30%）才立刻合并；否则等到 run 数攒到 4×lvl_t 再合并 —— 用读放大换写放大。' },
  { t: '合并：归并 + 继承 seq', d: '把最旧的一组 run 全键域 k 路归并，同键取 (seq, rid) 最大的那个。产物 seq = 源里最大的 seq —— 这样它仍比“没参与合并的更新 run”旧，旧值不会复活。' },
  { t: '门槛 20% vs 30%（本轮最大的一刀）', d: '20% 时，一堆 run 刚写下去就被覆盖，立刻又满足 20% 被重写一遍 —— 写下去的全是马上要扔的数据。提到 30% 后，写放大 3.14×→2.57×、耗时 0.95 s→0.40 s、段数峰值 23→7。' },
  { t: '单元压实：按性价比挑受害者', d: '每个 cell 独立算：dead ≥ 2 × live 才重写，且成本（live/PAGE+1 页）要落在本轮预算内（4~64 页，随本提交垃圾量自适应）。整条 run 都死了就不用重写 —— 直接摘链，零写回收。' },
  { t: '写放大花在哪', d: '同一负载（200k / 2000）：flush 4.08 MB + merge 3.32 MB + manifest 0.58 MB = 7.98 MB，活数据 3.10 MB ⇒ 2.57×。想再低就得牺牲读放大或空间放大。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">两个旋钮</div>
      <div className="stat-grid">
        <div><b>8 / 4</b><span>lvl_t / LVL_MAX</span></div>
        <div><b>30%</b><span>FCS_LVL_DIRTY_PCT</span></div>
        <div><b>2×</b><span>FCS_GC_RATIO</span></div>
        <div><b>4~64</b><span>压实预算（页）</span></div>
      </div>
      <div className="panel-title" style={{ marginTop: 14 }}>为什么不能更低</div>
      <ul className="bullets">
        <li>门槛降到 20%：写放大 3.14×，把“刚覆盖就重写”的活干了一遍</li>
        <li>门槛降到 0：每层每轮都重写，写放大接近 2× 的常驻</li>
        <li>GC_RATIO=1：空间 1.45× 但写 5.1×；=3：写 3.9× 空间 3.3×</li>
      </ul>
      <Note tone="violet">合并/压实全部是<b>机会性</b>的：NOSPACE 只跳过本轮，绝不把用户提交带崩（坑 #24）。</Note>
      {step >= 3 && <Note tone="green">实测：200k/2000 从 3.14× / 0.95 s → 2.57× / 0.36 s，读页 81 245 → 50 346。</Note>}
    </>
  )
}

const LEVELS = [
  { name: 'L0', runs: [1, 1, 1, 1, 1, 1], tone: '#7ff0ff' },
  { name: 'L1', runs: [3, 3, 3], tone: '#c4b5fd' },
  { name: 'L2', runs: [6, 2], tone: '#86efac' },
  { name: 'L3', runs: [6], tone: '#fde68a' },
]

export default function Scene({ step }) {
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={40} size={13}>LSM 分层（每层 run 数 ≤ lvl_t = 8）</Label>

      {LEVELS.map((lv, i) => {
        const y = 60 + i * 58
        const active = (step === 0 && i < 2) || (step === 1 && i === 0) || (step === 2 && i <= 1)
        return (
          <g key={lv.name}>
            <text x={36} y={y + 26} fontSize="12" fill={lv.tone} fontFamily="var(--mono)">{lv.name}</text>
            <rect x={70} y={y + 8} width={560} height={34} rx={6} fill="rgba(148,163,184,.04)" stroke="rgba(148,163,184,.18)" />
            {lv.runs.map((w, j) => (
              <g key={j}>
                <rect x={76 + j * 92} y={y + 10} width={w * 12 + 18} height={30} rx={4}
                  fill={active ? 'rgba(56,225,255,.16)' : 'rgba(148,163,184,.08)'}
                  stroke={active ? lv.tone : 'rgba(148,163,184,.30)'} />
                <text x={76 + j * 92 + (w * 12 + 18) / 2} y={y + 30} textAnchor="middle" fontSize="9.5"
                  fill={active ? lv.tone : '#8fa2c7'} fontFamily="var(--mono)">{w}p</text>
              </g>
            ))}
            {lv.runs.length >= 8 && <text x={640} y={y + 30} fontSize="10" fill="#fda4af" fontFamily="var(--mono)">≥ 8 ⇒ 升层</text>}
          </g>
        )
      })}
      {step >= 2 && (
        <g>
          <rect x={70} y={306} width={560} height={34} rx={6} fill="rgba(62,224,139,.10)" stroke="rgba(62,224,139,.5)" strokeDasharray="6 4" />
          <text x={86} y={328} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">
            合并 L0 最旧 6 条 → 1 条（seq = max(src)，lvl+1）
          </text>
          <path d="M 300 270 L 300 306" stroke="#86efac" strokeWidth="1.4" markerEnd="url(#ah)" />
          <text x={312} y={292} fontSize="10.5" fill="#86efac">产物进 L1</text>
        </g>
      )}

      {/* 右侧：门槛仪表 */}
      <Label x={672} y={40} size={12}>这一层：垃圾 / 生活字节</Label>
      <rect x={672} y={56} width={298} height={92} rx={9} fill="rgba(148,163,184,.05)" stroke="rgba(148,163,184,.25)" />
      <HBar x={700} y={72} w={240} h={14} value={step >= 3 ? 30 : 20} max={60} tone={step >= 3 ? 'green' : 'red'}
        right={step >= 3 ? '30%' : '20%'} label="阈值" />
      <text x={700} y={112} fontSize="10.5" fill="#c9d6f5" fontFamily="var(--mono)">
        实际垃圾占比 {step >= 3 ? '≈ 34%' : '≈ 22%'}
      </text>
      <text x={700} y={132} fontSize="10.5" fill={step >= 3 ? '#86efac' : '#fda4af'} fontFamily="var(--mono)">
        {step >= 3 ? '≥ 30% ⇒ 值得重写' : '≥ 20% ⇒ 立刻重写（但数据马上又死）'}
      </text>

      {/* 20 vs 30 对比 */}
      <g opacity={step >= 3 ? 1 : 0.3}>
        <Label x={672} y={190} size={12}>同一负载：门槛 20% vs 30%</Label>
        <HBar x={720} y={206} w={220} h={16} value={3.14} max={3.5} tone="red" label="20%" right="3.14×" />
        <HBar x={720} y={234} w={220} h={16} value={2.57} max={3.5} tone="green" label="30%" right="2.57×" delay={120} />
        <HBar x={720} y={268} w={220} h={16} value={0.95} max={1.0} tone="red" label="耗时" right="0.95 s" />
        <HBar x={720} y={296} w={220} h={16} value={0.36} max={1.0} tone="green" label="" right="0.36 s" delay={120} />
        <text x={720} y={332} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">段数峰值 23 → 7（读放大反而更好）</text>
      </g>

      {/* 写放大分解 */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <Label x={30} y={430} size={12}>写放大分解（200k / 2000，活数据 3.10 MB）</Label>
        {[
          { n: 'flush', v: 4.08, c: '#38e1ff', note: '1.32×' },
          { n: 'merge', v: 3.32, c: '#a78bfa', note: '1.07×' },
          { n: 'manifest', v: 0.58, c: '#ffc857', note: '0.19×' },
        ].map((r, i) => {
          const w = (r.v / 8.2) * 900
          const x = 30 + [0, 4.08, 7.40][i] / 8.2 * 900
          return (
            <g key={r.n}>
              <rect x={x} y={444} width={w} height={26} rx={4} fill={r.c} opacity="0.75" />
              <text x={x + w / 2} y={462} textAnchor="middle" fontSize={w > 90 ? 10.5 : 9} fill="#0b1020" fontFamily="var(--mono)" fontWeight="700">
                {w > 90 ? `${r.n} ${r.v}` : r.v}
              </text>
              <text x={x + w / 2} y={486} textAnchor="middle" fontSize="10" fill={r.c} fontFamily="var(--mono)">{r.n} {r.note}</text>
            </g>
          )
        })}
        <line x1={30 + (3.10 / 8.2) * 900} y1={444} x2={30 + (3.10 / 8.2) * 900} y2={492} stroke="#86efac" strokeDasharray="4 3" />
        <text x={30 + (3.10 / 8.2) * 900 + 6} y={508} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">活数据 3.10 MB</text>
        <text x={30} y={526} fontSize="11" fill="#c9d6f5" fontFamily="var(--mono)">
          合计 7.98 MB / 3.10 MB = 2.57×；再往下压只能牺牲读放大或空间放大。
        </text>
      </g>

      {/* 压实 */}
      <g opacity={step >= 4 ? 1 : 0.25}>
        <rect x={30} y={540} width={940} height={46} rx={9} fill="rgba(167,139,250,.06)" stroke="rgba(167,139,250,.3)" />
        <text x={46} y={559} fontSize="11" fill="#c4b5fd" fontFamily="var(--mono)">
          单元压实：dead ≥ 2 × live 且 cost ≤ budget ⇒ 整 cell 重写；整条 run 全死 ⇒ 零写摘链（retire）
        </text>
        <text x={46} y={576} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          重写是唯一能回收“旧版本还占着页”的手段：前缀合并永远回收不了空间（坑 #8）。
        </text>
      </g>
    </g>
  )
}
