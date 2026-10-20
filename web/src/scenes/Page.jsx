import React from 'react'
import { Defs, Label, Box, Note, KV } from '../ui.jsx'

export const STEPS = [
  { t: '一页 = 40 B 头 + 记录 + 空闲', d: '4 KB 是唯一的物理单位。页头 40 B，记录从 40 开始顺序排，used 指向尾部；剩下的空间当轮作废。' },
  { t: '页头 40 B 逐字段', d: 'kind / next / chk / ver / first / last / nrec / used / pfx / fmt。first,last 是这一页的键域 —— 读的时候用它做 fence 二分，不用解整页。' },
  { t: '记录 = 10 B 头 + 值', d: 'key(8) + len(2)。len 取偶数对齐，所以值是 2 B 对齐的；len=0 是墓碑（删除），它也要占一条记录。' },
  { t: '页内前缀压缩（fmt=1）', d: '封页时算本页首键与末键的公共高位字节数 pfx（0..7），每条记录只留 8-pfx 字节的键后缀。自增键/时间戳能省 4~7 B/条；随机键 pfx=0，和旧格式逐字节相同。' },
  { t: 'CRC 覆盖 12..used', d: 'chk = crc32c(page + 12, used - 12)：跳过 kind/next/chk 自己，从 ver 一直算到 used。硬件 crc32 指令 7.5 GB/s，4 KB 一页 0.6 µs（查表要 11 µs）。' },
  { t: '尾页不满就不占页', d: 'finish_run 封最后一张页时，如果它是空的或没写满就 pop 掉 —— 页号是稀缺资源，宁可少一页。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">页头 40 B</div>
      <div className="field-grid">
        {[
          ['0', 'kind', 'u32', 'PK_*'],
          ['4', 'next', 'u32', '同 run 下一页'],
          ['8', 'chk', 'u32', 'CRC32C'],
          ['12', 'ver', 'u32', '逻辑版本'],
          ['16', 'first', 'u64', '本页首键'],
          ['24', 'last', 'u64', '本页末键'],
          ['32', 'nrec', 'u16', '记录数'],
          ['34', 'used', 'u16', '有效字节'],
          ['36', 'pfx', 'u8', '共享高位字节 0..7'],
          ['37', 'fmt', 'u8', '1=前缀压缩'],
          ['38', 'pad', 'u16', '—'],
        ].map(([off, name, ty, note]) => (
          <div className={`fld ${step === 1 ? 'on' : ''}`} key={name}>
            <span className="fld-off">{off}</span>
            <span className="fld-name">{name}</span>
            <span className="fld-ty">{ty}</span>
            <span className="fld-note">{note}</span>
          </div>
        ))}
      </div>
      <div className="panel-title" style={{ marginTop: 14 }}>记录开销</div>
      <div className="code">size = 2 + ((len+1)&amp;~1) + 8 - pfx
      pfx=0 → 10 B 头（旧格式）
      pfx=6 → 4 B 头（有序键）</div>
      <Note tone="amber">页内只做「同一页内」的前缀共享，不跨页、不改语义；旧库 pfx=0 直接走原路径。</Note>
    </>
  )
}

const PAGE_X = 40, PAGE_Y = 70, PAGE_W = 920, PAGE_H = 118

function Rec({ x, w, k, pfx, active, tone = 'cyan' }) {
  const sw = 8 - pfx
  return (
    <g opacity={active === false ? 0.45 : 1}>
      <rect x={x} y={PAGE_Y + 34} width={w} height={44} rx={5}
        fill={active ? 'rgba(56,225,255,.16)' : 'rgba(56,225,255,.05)'}
        stroke={active ? '#7ff0ff' : 'rgba(56,225,255,.25)'} />
      <text x={x + w / 2} y={PAGE_Y + 54} textAnchor="middle" fontSize="10.5" fill="#7ff0ff" fontFamily="var(--mono)">
        {sw} B 键后缀
      </text>
      <text x={x + w / 2} y={PAGE_Y + 69} textAnchor="middle" fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
        +2 len + 值
      </text>
    </g>
  )
}

export default function Scene({ step }) {
  const pfx = step >= 3 ? 6 : 0
  const sw = 8 - pfx
  
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={40} y={40} size={13}>一个 4 KB 页（run 内不可变）</Label>

      {/* 页条 */}
      <rect x={PAGE_X} y={PAGE_Y} width={PAGE_W} height={PAGE_H} rx={10} fill="rgba(10,16,32,.75)" stroke="rgba(148,163,184,.35)" />
      {/* 页头 */}
      <rect x={PAGE_X + 2} y={PAGE_Y + 2} width={110} height={PAGE_H - 4} rx={7}
        fill={step === 1 ? 'rgba(255,200,87,.20)' : 'rgba(255,200,87,.08)'}
        stroke={step === 1 ? '#fde68a' : 'rgba(255,200,87,.4)'} />
      <text x={PAGE_X + 57} y={PAGE_Y + PAGE_H / 2 - 6} textAnchor="middle" fontSize="12" fill="#fde68a" fontFamily="var(--mono)">DHDR</text>
      <text x={PAGE_X + 57} y={PAGE_Y + PAGE_H / 2 + 12} textAnchor="middle" fontSize="11" fill="#c9a34a" fontFamily="var(--mono)">40 B</text>

      {/* 记录 */}
      {[0, 1, 2, 3].map((i) => (
        <Rec key={i} x={PAGE_X + 122 + i * 132} w={120} k={i} pfx={pfx}
          active={step === 2 ? i < 2 : step === 3 ? i < 3 : undefined} tone="cyan" />
      ))}
      {/* 空闲 */}
      <rect x={PAGE_X + 650} y={PAGE_Y + 34} width={PAGE_W - 660} height={44} rx={5}
        fill="rgba(148,163,184,.05)" stroke="rgba(148,163,184,.25)" strokeDasharray="5 5" />
      <text x={PAGE_X + 650 + (PAGE_W - 660) / 2} y={PAGE_Y + 61} textAnchor="middle" fontSize="11" fill="#8fa2c7">空闲（本轮作废）</text>

      {/* used 指针 */}
      <line x1={PAGE_X + 650} y1={PAGE_Y + 20} x2={PAGE_X + 650} y2={PAGE_Y + PAGE_H - 8} stroke="#3ee08b" strokeWidth="1.4" strokeDasharray="4 3" />
      <text x={PAGE_X + 654} y={PAGE_Y + 18} fontSize="11" fill="#86efac" fontFamily="var(--mono)">used</text>

      {/* 页头放大：字段条 */}
      {step >= 1 && (
        <g style={{ transition: 'opacity .4s' }}>
          <Label x={40} y={225} size={12}>页头放大（字节偏移）</Label>
          {(() => {
            const segs = [
              [0, 4, 'kind', 'amber'], [4, 4, 'next', 'amber'], [8, 4, 'chk', 'red'], [12, 4, 'ver', 'amber'],
              [16, 8, 'first', 'cyan'], [24, 8, 'last', 'cyan'], [32, 2, 'nrec', 'violet'], [34, 2, 'used', 'green'],
              [36, 1, 'pfx', 'violet'], [37, 1, 'fmt', 'violet'], [38, 2, 'pad', 'slate'],
            ]
            const unit = segs.map(([off, len, name]) => Math.max(len * 1.55, name.length * 1.15 + 1.4))
            const total = unit.reduce((a, b) => a + b, 0)
            let acc = 0
            return segs.map(([off, len, name, tone], i) => {
              const x = 40 + (acc / total) * 920
              const w = (unit[i] / total) * 920
              acc += unit[i]
              const g = { amber: '#fde68a', red: '#fda4af', cyan: '#7ff0ff', violet: '#c4b5fd', green: '#86efac', slate: '#94a3b8' }[tone]
              return (
                <g key={name}>
                  <rect x={x} y={238} width={w - 3} height={34} rx={4} fill="rgba(148,163,184,.06)" stroke={g} strokeOpacity="0.65" />
                  <text x={x + (w - 3) / 2} y={256} textAnchor="middle" fontSize="10.5" fill={g} fontFamily="var(--mono)">{name}</text>
                  <text x={x + (w - 3) / 2} y={268} textAnchor="middle" fontSize="9" fill="#8fa2c7" fontFamily="var(--mono)">{off}</text>
                </g>
              )
            })
          })()}
        </g>
      )}

      {/* 记录放大 */}
      {step >= 2 && (
        <g>
          <Label x={40} y={316} size={12}>
            {step >= 3 ? `记录（pfx = ${pfx}：键的高 ${pfx} 字节只在页首存一次）` : '记录（pfx = 0，旧格式逐字节相同）'}
          </Label>
          {(() => {
            const rows = [
              { k: '0x00000000000003E9', tag: '页首键 → 存 8 B', w: 8 * 17 },
              { k: '0x03EA', tag: pfx ? '同前缀 6 B，只存 2 B' : '存 8 B', w: pfx ? 52 : 8 * 17 },
              { k: '0x03EB', tag: pfx ? '同前缀 6 B，只存 2 B' : '存 8 B', w: pfx ? 52 : 8 * 17 },
            ]
            return rows.map((r, i) => {
              const y = 336 + i * 46
              return (
                <g key={i}>
                  <rect x={40} y={y} width={r.w} height={30} rx={4} fill="rgba(56,225,255,.12)" stroke="rgba(56,225,255,.5)" />
                  <text x={40 + r.w / 2} y={y + 20} textAnchor="middle" fontSize="10" fill="#7ff0ff" fontFamily="var(--mono)">
                    {pfx && i > 0 ? r.k : r.k}
                  </text>
                  <rect x={46 + r.w} y={y} width={34} height={30} rx={4} fill="rgba(167,139,250,.12)" stroke="rgba(167,139,250,.5)" />
                  <text x={63 + r.w} y={y + 20} textAnchor="middle" fontSize="10" fill="#c4b5fd" fontFamily="var(--mono)">len</text>
                  <rect x={84 + r.w} y={y} width={110} height={30} rx={4} fill="rgba(62,224,139,.10)" stroke="rgba(62,224,139,.4)" />
                  <text x={139 + r.w} y={y + 20} textAnchor="middle" fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">值（2 B 对齐）</text>
                  <text x={210 + r.w} y={y + 20} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">{r.tag}</text>
                </g>
              )
            })
          })()}
        </g>
      )}

      {/* CRC */}
      {step >= 4 && (
        <g>
          <rect x={40} y={480} width={430} height={86} rx={10} fill="rgba(255,93,115,.07)" stroke="rgba(255,93,115,.45)" />
          <text x={56} y={504} fontSize="12" fill="#fda4af" fontFamily="var(--mono)">chk = crc32c(page + 12, used - 12)</text>
          <text x={56} y={524} fontSize="11" fill="#c9d6f5">覆盖 ver 之后的全部有效字节；坏一页 ⇒ 读失败，不会读出错值。</text>
          <text x={56} y={546} fontSize="11" fill="#8fa2c7">硬件 crc32 指令：4 KB / 0.6 µs；查表实现要 11 µs（差 20 倍）。</text>
        </g>
      )}

      {/* 尾页 */}
      {step >= 5 && (
        <g>
          <rect x={500} y={480} width={460} height={86} rx={10} fill="rgba(62,224,139,.07)" stroke="rgba(62,224,139,.4)" />
          <text x={516} y={504} fontSize="12" fill="#86efac" fontFamily="var(--mono)">finish_run: 尾页空/未用满 ⇒ pop 掉</text>
          <text x={516} y={524} fontSize="11" fill="#c9d6f5">页号是稀缺资源：一批 3 条记录不会占掉 2 个页号。</text>
          <text x={516} y={546} fontSize="11" fill="#8fa2c7">封页时才做前缀压缩 —— 因为 pfx 要看本页首末键。</text>
        </g>
      )}
    </g>
  )
}
