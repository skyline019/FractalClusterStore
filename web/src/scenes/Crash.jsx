import React from 'react'
import { Defs, Label, Note } from '../ui.jsx'

export const STEPS = [
  { t: '写出顺序本身就是崩溃安全协议', d: '数据页 → 清单页 → 超级块 → sync。每一刀掉电都只会落在“完整旧态”或“完整新态”之间，没有第三种可能。' },
  { t: '掉电点在数据页：回到旧态', d: '页写了一半？那页还挂在旧清单上（或根本没挂上），旧清单指向的页仍是完好的。超级块没动 ⇒ 整个事务没有发生。' },
  { t: '掉电点在超级块：双槽救场', d: '超级块有 A/B 两个槽，每次只写另一个。写坏一个，挂载时 CRC 校验失败 ⇒ 用另一个（vers 小一版）= 旧态。' },
  { t: 'NOR：只能 1→0，先擦后写', d: 'NOR 设备的位只能从 1 变 0，写超级块前必须先擦整个槽。擦除本身要放在提交点之后 —— 否则擦完掉电，两版都没了。' },
  { t: '损坏检测：页 CRC + 清单 CRC', d: '每页 chk 覆盖 12..used；清单页 CRC 覆盖 MHDR..n*40。校验不过 ⇒ 读失败/拒绝这一版清单，绝不“读出错误数据”。' },
  { t: '重放 + 精确重算 + 回归测试', d: '挂载：选槽 → wait_io → 重放清单 → 丢弃越界条目 → recompute_liveness（按段重算每条 run 的存活账）。f3 逐写入点掉电注入，r2 覆盖满库/异步/注入/回退/卡死五种事故。' },
]

export function SIDE() {
  return (
    <>
      <div className="panel-title">掉电注入（f3）</div>
      <p className="mini-p">每一次页写、每一次超级块写都当成可能的“最后一刀”，在那一刀之后重开，断言：</p>
      <ul className="bullets">
        <li>数据要么是提交前的完整视图</li>
        <li>要么是提交后的完整视图</li>
        <li>页 CRC 坏 ⇒ 必须被发现</li>
        <li>清单坏 ⇒ 必须拒绝挂载</li>
      </ul>
      <div className="panel-title" style={{ marginTop: 14 }}>r2 的五种事故</div>
      <div className="mini-list">
        {['满库（96 页小设备）', '异步写 + 灌满', '每 7 次提交坏一笔写', '回退后重挂载', '设备卡 3 s（有界等待）'].map((t, i) => (
          <div className="mini-row" key={i}><span className="mini-no">{i + 1}</span><span className="mini-t">{t}</span></div>
        ))}
      </div>
      <Note tone="red">回退不是“回到历史”：<b>vers_ = 回退前时钟 + 1</b>。否则另一个槽里版本更高的旧分支会在下次挂载时胜出，回退被静默复活（坑 #32）。</Note>
    </>
  )
}

export default function Scene({ step }) {
  const cuts = [
    { x: 190, label: '数据页写到一半', outcome: '旧态', tone: '#fda4af' },
    { x: 420, label: '清单页写坏', outcome: '旧态', tone: '#fda4af' },
    { x: 640, label: '超级块写坏', outcome: '旧态（另一槽）', tone: '#fde68a' },
    { x: 830, label: 'sync 之后', outcome: '新态', tone: '#86efac' },
  ]
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={40} size={13}>一次提交的时间轴（✂ = 掉电注入点）</Label>

      {[
        { x: 60, w: 150, t: '数据页', s: '可异步 · 多页' },
        { x: 250, w: 150, t: '清单页', s: '增量 / 快照' },
        { x: 440, w: 150, t: '超级块', s: '双槽 + CRC' },
        { x: 630, w: 130, t: 'sync()', s: '持久化屏障' },
      ].map((b, i) => (
        <g key={b.t}>
          <rect x={b.x} y={70} width={b.w} height={52} rx={8}
            fill={step >= i ? 'rgba(56,225,255,.12)' : 'rgba(148,163,184,.05)'}
            stroke={step >= i ? 'rgba(56,225,255,.5)' : 'rgba(148,163,184,.25)'} />
          <text x={b.x + b.w / 2} y={94} textAnchor="middle" fontSize="12" fill="#c9d6f5" fontFamily="var(--mono)">{b.t}</text>
          <text x={b.x + b.w / 2} y={112} textAnchor="middle" fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">{b.s}</text>
        </g>
      ))}
      <line x1="60" y1="150" x2="930" y2="150" stroke="rgba(148,163,184,.4)" />

      {cuts.map((c, i) => (
        <g key={i} opacity={step === 0 || step === i + 1 ? 1 : 0.35}>
          <line x1={c.x} y1={140} x2={c.x} y2={176} stroke={c.tone} strokeWidth="1.6" strokeDasharray="4 3" />
          <text x={c.x} y={136} textAnchor="middle" fontSize="13" fill={c.tone}>✂</text>
          <text x={c.x} y={192} textAnchor="middle" fontSize="10" fill="#c9d6f5">{c.label}</text>
          <text x={c.x} y={208} textAnchor="middle" fontSize="10" fill={c.tone} fontFamily="var(--mono)">⇒ {c.outcome}</text>
        </g>
      ))}

      {/* 双槽 */}
      <g opacity={step >= 2 ? 1 : 0.3}>
        <Label x={30} y={250} size={12}>超级块两个槽（乒乓）</Label>
        {[0, 1].map((i) => {
          const bad = step >= 3 && i === 1
          return (
            <g key={i}>
              <rect x={30 + i * 300} y={264} width={280} height={86} rx={9}
                fill={bad ? 'rgba(255,93,115,.08)' : 'rgba(62,224,139,.06)'}
                stroke={bad ? 'rgba(255,93,115,.55)' : 'rgba(62,224,139,.4)'} />
              <text x={46 + i * 300} y={288} fontSize="11.5" fill="#c9d6f5" fontFamily="var(--mono)">
                槽 {i ? 'B' : 'A'} · vers {i ? 40 : 41}
              </text>
              <text x={46 + i * 300} y={308} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
                man_last={i ? 9 : 10} · inline {i ? '31/32' : '29/32'}
              </text>
              <text x={46 + i * 300} y={330} fontSize="11" fill={bad ? '#fda4af' : '#86efac'} fontFamily="var(--mono)">
                crc = {bad ? 'BAD → 跳过' : 'OK'}
              </text>
            </g>
          )
        })}
        <text x={650} y={290} fontSize="11" fill="#86efac">挂载：选 CRC 正确且 vers 最大</text>
        <text x={650} y={312} fontSize="11" fill="#8fa2c7">最坏情况只丢最后一次提交</text>
        <text x={650} y={334} fontSize="11" fill="#8fa2c7">不会出现“半个事务”</text>
      </g>

      {/* NOR */}
      <g opacity={step >= 3 ? 1 : 0.25}>
        <rect x={30} y={376} width={452} height={92} rx={9} fill="rgba(255,200,87,.05)" stroke="rgba(255,200,87,.3)" />
        <text x={46} y={400} fontSize="12" fill="#fde68a" fontFamily="var(--mono)">NOR：1 → 0 单向</text>
        <text x={46} y={420} fontSize="10.5" fill="#c9d6f5">写之前必须先擦整个槽；擦除本身安排在提交点之后。</text>
        <text x={46} y={438} fontSize="10.5" fill="#c9d6f5">擦完掉电 ⇒ 另一个槽仍是完整旧态。</text>
        <text x={46} y={456} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">f3 专门验这条：数据页只写一次、超级块只擦不写。</text>
      </g>

      {/* 损坏检测 */}
      <g opacity={step >= 4 ? 1 : 0.25}>
        <rect x={498} y={376} width={472} height={92} rx={9} fill="rgba(255,93,115,.05)" stroke="rgba(255,93,115,.3)" />
        <text x={514} y={400} fontSize="12" fill="#fda4af" fontFamily="var(--mono)">CRC 是“发现”，不是“修复”</text>
        <text x={514} y={420} fontSize="10.5" fill="#c9d6f5">页 chk 覆盖 page+12..used；清单 chk 覆盖 MHDR..n*40。</text>
        <text x={514} y={438} fontSize="10.5" fill="#c9d6f5">校验不过 ⇒ RC::CORRUPT / 拒绝这一版清单。</text>
        <text x={514} y={456} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">坏条目（npages 越界）一律丢弃，挂载不失败。</text>
      </g>

      {/* 重放 */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={30} y={486} width={940} height={96} rx={9} fill="rgba(62,224,139,.05)" stroke="rgba(62,224,139,.3)" />
        <text x={46} y={510} fontSize="12" fill="#86efac" fontFamily="var(--mono)">挂载后的第一件事：把账重算一遍</text>
        {['选槽（CRC + vers）', 'wait_io 排空', '重放清单链', '丢弃坏条目', 'recompute_liveness', '逐单元 verify'].map((t, i) => (
          <g key={t}>
            <rect x={46 + i * 152} y={524} width={142} height={38} rx={6} fill="rgba(148,163,184,.06)" stroke="rgba(148,163,184,.28)" />
            <text x={46 + i * 152 + 71} y={548} textAnchor="middle" fontSize="10" fill="#c9d6f5" fontFamily="var(--mono)">{t}</text>
          </g>
        ))}
      </g>
    </g>
  )
}
