import React from 'react'
import { Defs, Label, Note } from '../ui.jsx'
import REAL from '../realpage.json'

export const STEPS = [
  { t: '真的盘上字节：数据页 #34', d: '下面的 96 个字节不是画出来的，是 web/dump.cpp 跑完 40 次提交后直接从 RamDev 里 memcpy 出来的。' },
  { t: '页头 40 B 逐字段对号', d: 'kind=DATA(0x44415441)、next=NIL、chk=0x8C23A72A、ver=32、first=1000、last=2292、nrec=100、used=1274、pfx=6、fmt=1。' },
  { t: '记录：16 B 一条（含前缀压缩）', d: '每条 = 2 B 键后缀 + 2 B len + 12 B 值 = 16 B。若 pfx=0 同一条要 20 B —— 有序键省下的就是这 4 B/条。' },
  { t: '超级块两槽（vers 42 / 41）', d: '槽 A 的 vers 大 1 ⇒ 挂载选 A。两个槽的 man_last 都指向 37，n_inl 11 / 10 —— 说明本次提交的增量还在内联区，nman=3 页是更早溢出出来的。' },
  { t: '清单页 #37：链头 + 32 条', d: 'prev=36 指回旧页；n=32 条；那条 op=2 是 M_RUN_DEL —— 前 40 次提交把老 run 一条条摘掉，正好落在链头这一页。' },
  { t: '口径：这些数能对上', d: 'used_pages=38、cells=1、runs=11、recs=130、live=1564 B、verify errors=0 —— 页里的字节和内存里的账逐项吻合。' },
]

export function SIDE() {
  return (
    <>
      <div className="panel-title">怎么复现</div>
      <div className="code">g++ -std=c++17 -O2 -Iinclude \
  web/dump.cpp -o dump
./dump &gt; src/realpage.json</div>
      <p className="mini-p" style={{ marginTop: 8 }}>
        程序：10 条写入 + 覆盖 + 墓碑 + 40 次小提交（把超级块内联区撑爆），然后把超级块、清单页、最新数据页的原始字节 dump 成 JSON。
      </p>
      <div className="panel-title" style={{ marginTop: 14 }}>页头字段（真实值）</div>
      <div className="field-grid">
        {[
          ['0', 'kind', 'DATA'], ['4', 'next', 'NIL'], ['8', 'chk', '0x8C23A72A'], ['12', 'ver', '32'],
          ['16', 'first', '1000'], ['24', 'last', '2292'], ['32', 'nrec', '100'],
          ['34', 'used', '1274'], ['36', 'pfx', '6'], ['37', 'fmt', '1'],
        ].map(([o, n, v]) => (
          <div className="fld" key={n}>
            <span className="fld-off">{o}</span><span className="fld-name">{n}</span>
            <span className="fld-ty">{v}</span>
          </div>
        ))}
      </div>
      <Note tone="green">这页有 100 条记录、1274 B 有效数据、pfx=6：换成 pfx=0 需要 40 + 100×20 = 2040 B，压缩后只占 1274 B。</Note>
    </>
  )
}

const TONE = [
  // [start, end, tone]
  [0, 4, '#ffc857'], [4, 8, '#ffc857'], [8, 12, '#ff5d73'], [12, 16, '#ffc857'],
  [16, 24, '#38e1ff'], [24, 32, '#38e1ff'], [32, 34, '#a78bfa'], [34, 36, '#3ee08b'],
  [36, 37, '#a78bfa'], [37, 38, '#a78bfa'], [38, 40, '#64748b'],
]

function toneOf(i) {
  if (i >= 40) return '#7ff0ff'
  for (const [a, b, c] of TONE) if (i >= a && i < b) return c
  return '#64748b'
}

function HexDump({ x, y, bytes, rows = 6, cols = 8, cw = 25, hi = true }) {
  return (
    <g>
      {Array.from({ length: rows }).map((_, r) => (
        <g key={r}>
          <text x={x} y={y + r * 20} fontSize="10.5" fill="#64748b" fontFamily="var(--mono)">
            {String(r * cols).padStart(3, '0')}
          </text>
          {Array.from({ length: cols }).map((_, c) => {
            const i = r * cols + c
            const v = bytes.slice(i * 2, i * 2 + 2)
            const on = hi && i >= 0 && i < 40
            return (
              <text key={c} x={x + 32 + c * cw} y={y + r * 20} fontSize="11"
                fill={on ? toneOf(i) : i >= 40 ? '#8fa2c7' : '#64748b'} fontFamily="var(--mono)">
                {v}
              </text>
            )
          })}
        </g>
      ))}
    </g>
  )
}

export default function Scene({ step }) {
  const d = REAL.data
  const sbA = REAL.sb[0], sbB = REAL.sb[1]
  const man = REAL.man
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={40} size={13}>
        数据页 #{d.pid} 的前 96 字节（真实 dump：nrec={d.nrec} · used={d.used} · pfx={d.pfx} · fmt={d.fmt}）
      </Label>

      <rect x={26} y={56} width={520} height={150} rx={9} fill="rgba(4,8,18,.65)" stroke="rgba(148,163,184,.25)" />
      <HexDump x={34} y={80} bytes={d.hex} />

      {step >= 1 && (
        <g>
          <Label x={566} y={70} size={12}>字段对号（偏移 → 值）</Label>
          {[
            ['0..3', 'kind = 0x44415441 ("DATA")', '#ffc857'],
            ['4..7', 'next = 0xFFFFFFFF（本 run 只有一页）', '#ffc857'],
            ['8..11', 'chk = 0x8C23A72A（crc32c of 12..used）', '#ff5d73'],
            ['12..15', 'ver = 32', '#ffc857'],
            ['16..23', 'first = 1000（本页首键）', '#38e1ff'],
            ['24..31', 'last = 2292（本页末键）', '#38e1ff'],
            ['32..33', 'nrec = 100', '#a78bfa'],
            ['34..35', 'used = 1274', '#3ee08b'],
            ['36', `pfx = ${d.pfx}（共享 6 字节高位）`, '#a78bfa'],
            ['37', `fmt = ${d.fmt}（前缀压缩开启）`, '#a78bfa'],
          ].map((r, i) => (
            <g key={i}>
              <text x={566} y={92 + i * 17} fontSize="10.5" fill="#64748b" fontFamily="var(--mono)">{r[0]}</text>
              <text x={618} y={92 + i * 17} fontSize="10.5" fill={r[2]} fontFamily="var(--mono)">{r[1]}</text>
            </g>
          ))}
        </g>
      )}

      {step >= 2 && (
        <g>
          <Label x={30} y={236} size={12}>记录解码（pfx=6 ⇒ 键后缀 2 B）</Label>
          {d.recs.map((r, i) => (
            <g key={i}>
              <rect x={30 + i * 240} y={250} width={224} height={62} rx={7} fill="rgba(56,225,255,.06)" stroke="rgba(56,225,255,.35)" />
              <text x={44 + i * 240} y={272} fontSize="10.5" fill="#7ff0ff" fontFamily="var(--mono)">off {r.off} → key {r.key}</text>
              <text x={44 + i * 240} y={290} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">len {r.len} · 占 {r.size} B</text>
              <text x={44 + i * 240} y={306} fontSize="10" fill="#86efac" fontFamily="var(--mono)">2 + {r.len} + (8-6) = {r.size}</text>
            </g>
          ))}
        </g>
      )}

      {step >= 3 && (
        <g>
          <Label x={30} y={348} size={12}>超级块两槽</Label>
          {[sbA, sbB].map((s, i) => (
            <g key={i}>
              <rect x={30 + i * 330} y={362} width={310} height={70} rx={8}
                fill={i === 0 ? 'rgba(255,200,87,.08)' : 'rgba(148,163,184,.05)'}
                stroke={i === 0 ? 'rgba(255,200,87,.55)' : 'rgba(148,163,184,.25)'} />
              <text x={44 + i * 330} y={384} fontSize="10.5" fill="#fde68a" fontFamily="var(--mono)">
                槽 {i ? 'B' : 'A'} · vers {s.vers} · {s.note}
              </text>
              <text x={44 + i * 330} y={402} fontSize="10" fill="#c9d6f5" fontFamily="var(--mono)">
                tail {s.tail} · man_first {s.man_first} · man_last {s.man_last}
              </text>
              <text x={44 + i * 330} y={420} fontSize="10" fill="#c9d6f5" fontFamily="var(--mono)">
                nman {s.nman} · n_inl {s.n_inl} · chk 0x{s.chk.toString(16).toUpperCase()}
              </text>
            </g>
          ))}
        </g>
      )}

      {step >= 4 && (
        <g>
          <Label x={30} y={462} size={12}>清单页 #{man.pid}（prev={man.prev} · n={man.n}）</Label>
          {man.ents.map((e, i) => (
            <g key={i}>
              <rect x={30 + i * 316} y={474} width={300} height={62} rx={7} fill="rgba(167,139,250,.06)" stroke="rgba(167,139,250,.35)" />
              <text x={44 + i * 316} y={496} fontSize="10" fill="#c4b5fd" fontFamily="var(--mono)">
                op {e.op} {e.op === 2 ? '(M_RUN_DEL)' : e.op === 1 ? '(M_RUN_ADD)' : ''} · lvl {e.lvl}
              </text>
              <text x={44 + i * 316} y={514} fontSize="10" fill="#c9d6f5" fontFamily="var(--mono)">
                key {e.key}..{e.last} · page0 {e.page0} · np {e.npages}
              </text>
              <text x={44 + i * 316} y={530} fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">
                nbytes {e.nbytes} · seq {e.seq}
              </text>
            </g>
          ))}
        </g>
      )}

      {step >= 5 && (
        <g>
          <rect x={30} y={552} width={940} height={40} rx={8} fill="rgba(62,224,139,.07)" stroke="rgba(62,224,139,.35)" />
          <text x={46} y={577} fontSize="11" fill="#86efac" fontFamily="var(--mono)">
            used_pages {REAL.used_pages} · cells {REAL.report.cells} · runs {REAL.report.runs} · recs {REAL.report.recs} · live {REAL.report.live} B · errors {REAL.report.errors}
          </text>
        </g>
      )}
    </g>
  )
}
