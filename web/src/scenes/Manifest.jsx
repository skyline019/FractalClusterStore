import React from 'react'
import { Defs, Label, Box, Note, Arrow } from '../ui.jsx'

export const STEPS = [
  { t: '超级块：两个槽 A / B', d: '每次提交写另一个槽（乒乓），槽里带 vers_、CRC、清单头指针、内联增量。挂载时选“CRC 正确且 vers_ 最大”的那个 —— 掉电时至少有一个槽是完整的。' },
  { t: '内联增量：32 条以内不占页', d: '绝大多数提交只挂 1~2 条 run，直接写进超级块的 INLINE_MAX=32 个槽位，零额外页 I/O。满了才溢出成清单页。' },
  { t: '清单链：102 条/页', d: '每页 16 B 头（prev 指针 / n / CRC）+ 102 × 40 B 条目。条目 = (op, lvl, key, last, page0, npages, ver, nbytes, seq, pad)。新页指回旧页，形成链。' },
  { t: '链太长就写全量快照', d: '链长超过 MAN_SNAP_AT=64 时，直接写一份“当前所有 cell + 所有 run”的快照，然后一次性释放旧链 —— 挂载时只读快照，不用回放几十页。' },
  { t: '挂载 = 择优 + 重放 + 精确重算', d: '选槽 → 沿 prev 链从旧到新重放 → 同 (key, op) 后者胜 → 越界条目丢弃并计数 → 最后 recompute_liveness() 把每条 run 的存活账按段重算一遍。' },
  { t: '清单坏了也要能挂上', d: 'apply_man 里照着 npages 分配就是给坏数据一次申请 9 GB 的机会。越界条目一律丢弃 + 计数；CRC 坏掉的清单页直接拒绝加载这一版，回落到另一个槽。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">清单页布局</div>
      <div className="code">MHDR 16 B
  +0  prev 页号 (u32)
  +4  保留
  +8  n   条目数 (u32)
  +12 crc32c(MHDR, n*MENT)
每条 40 B：
  op/lvl | key u64 | last u64
  page0 u32 | npages u32 | ver u32
  nbytes u32 | seq u32 | pad</div>
      <div className="panel-title" style={{ marginTop: 14 }}>容量</div>
      <div className="stat-grid">
        <div><b>102</b><span>条/页</span></div>
        <div><b>32</b><span>内联增量条数</span></div>
        <div><b>64</b><span>链长上限后快照</span></div>
        <div><b>2</b><span>超级块槽</span></div>
      </div>
      {step >= 4 && <Note tone="green">挂载时先 wait_io，再按盘重建 —— 在飞的旧页写绝不能落进新结构（坑 #25）。</Note>}
    </>
  )
}

export default function Scene({ step }) {
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={40} size={13}>超级块（2 个槽，乒乓写）</Label>

      {[0, 1].map((i) => {
        const good = i === 0 || step >= 5 // 槽 A 无效只在最后一步演示
        const active = (step === 0) || (step >= 4 && i === 0)
        return (
          <g key={i}>
            <rect x={30 + i * 330} y={56} width={306} height={124} rx={10}
              fill={active ? 'rgba(255,200,87,.10)' : 'rgba(148,163,184,.05)'}
              stroke={good ? (active ? '#fde68a' : 'rgba(255,200,87,.45)') : 'rgba(255,93,115,.55)'}
              strokeWidth={active ? 1.8 : 1} />
            <text x={46 + i * 330} y={80} fontSize="12" fill="#fde68a" fontFamily="var(--mono)">
              槽 {i === 0 ? 'A' : 'B'}
            </text>
            <text x={46 + i * 330} y={102} fontSize="11" fill="#c9d6f5" fontFamily="var(--mono)">vers = {i === 0 ? '41' : '40'}</text>
            <text x={46 + i * 330} y={122} fontSize="11" fill="#c9d6f5" fontFamily="var(--mono)">
              man_last = {i === 0 ? '10' : '9'} · nman = 6
            </text>
            <text x={46 + i * 330} y={142} fontSize="11" fill="#c9d6f5" fontFamily="var(--mono)">inline = {i === 0 ? '29/32' : '31/32'}</text>
            <text x={46 + i * 330} y={164} fontSize="11" fill={good ? '#86efac' : '#fda4af'} fontFamily="var(--mono)">
              crc = {good ? 'OK' : 'BAD'}
            </text>
            {!good && <text x={200 + i * 330} y={164} fontSize="11" fill="#fda4af" fontFamily="var(--mono)">← 掉电写坏</text>}
          </g>
        )
      })}
      <Arrow d="M 196 190 L 196 214" tone="amber" active={step === 0} />
      <text x={206} y={208} fontSize="10.5" fill="#8fa2c7">选中 vers 最大且 CRC 正确者</text>

      {/* 内联增量 */}
      <Label x={30} y={240} size={12}>超级块内联增量（INLINE_MAX = 32）</Label>
      <g>
        {Array.from({ length: 32 }).map((_, i) => (
          <rect key={i} x={30 + i * 29} y={252} width={24} height={30} rx={3}
            fill={i < (step >= 1 ? 29 : 29) ? 'rgba(255,200,87,.22)' : 'rgba(148,163,184,.06)'}
            stroke={i < 29 ? 'rgba(255,200,87,.55)' : 'rgba(148,163,184,.25)'} />
        ))}
        <text x={30} y={302} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">0</text>
        <text x={940} y={302} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">31</text>
        <text x={420} y={272} fontSize="11" fill="#fde68a" fontFamily="var(--mono)">已用 29 条（一次提交通常只 +1~2 条 run）</text>
      </g>

      {/* 清单链 */}
      <Label x={30} y={340} size={12}>清单链（新页 —— prev —— 旧页）</Label>
      {[0, 1, 2].map((i) => {
        const x = 30 + i * 250
        const active = step >= 2
        return (
          <g key={i} opacity={active ? 1 : 0.3}>
            <rect x={x} y={352} width={214} height={110} rx={8}
              fill="rgba(56,225,255,.06)" stroke={active ? 'rgba(56,225,255,.55)' : 'rgba(148,163,184,.25)'} />
            <text x={x + 12} y={374} fontSize="10.5" fill="#7ff0ff" fontFamily="var(--mono)">
              MAN page {i === 0 ? 'head' : i === 1 ? 'prev' : 'prev'}
            </text>
            <text x={x + 12} y={392} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">n = {i === 0 ? 6 : 102}</text>
            {Array.from({ length: 6 }).map((_, r) => (
              <rect key={r} x={x + 12} y={400 + r * 9} width={190 - r * 22} height={5} rx={2} fill="rgba(56,225,255,.30)" />
            ))}
            <text x={x + 12} y={452} fontSize="9.5" fill="#8fa2c7" fontFamily="var(--mono)">crc32c(MHDR, n*40)</text>
          </g>
        )
      })}
      {[0, 1].map((i) => (
        <Arrow key={i} d={`M ${30 + i * 250 + 216} ${407} L ${30 + (i + 1) * 250 - 4} ${407}`} tone="cyan" active={step >= 2} />
      ))}
      <text x={770} y={372} fontSize="11" fill="#8fa2c7">链长 &gt; 64 ⇒</text>
      <text x={770} y={392} fontSize="11" fill="#fde68a" fontFamily="var(--mono)">写全量快照</text>
      <text x={770} y={410} fontSize="10.5" fill="#8fa2c7">（所有 cell + 所有 run）</text>
      <text x={770} y={428} fontSize="10.5" fill="#8fa2c7">再释放整条旧链</text>
      <text x={770} y={452} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">挂载只读 1 遍快照</text>

      {/* 挂载流程 */}
      <g opacity={step >= 4 ? 1 : 0.25}>
        <Label x={30} y={500} size={12}>挂载 / 恢复流程</Label>
        {['选槽（CRC+vers）', 'wait_io 排空', '重放清单链', '坏条目丢弃+计数', 'recompute_liveness'].map((t, i) => (
          <g key={t}>
            <rect x={30 + i * 190} y={514} width={176} height={44} rx={8} fill="rgba(62,224,139,.08)" stroke="rgba(62,224,139,.4)" />
            <text x={30 + i * 190 + 88} y={541} textAnchor="middle" fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">{t}</text>
            {i < 4 && <Arrow d={`M ${30 + i * 190 + 178} 536 L ${30 + (i + 1) * 190 - 2} 536`} tone="green" arrow={false} dash="3 3" />}
          </g>
        ))}
      </g>
      {step >= 5 && (
        <text x={30} y={588} fontSize="11" fill="#fda4af" fontFamily="var(--mono)">
          槽 A CRC 坏 ⇒ 回落槽 B（vers 40）；清单页 CRC 坏 ⇒ 拒绝这一版；条目越界 ⇒ 丢弃，不影响挂载。
        </text>
      )}
    </g>
  )
}
