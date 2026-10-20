import React from 'react'
import { Defs, Label, Box, Note, Arrow, Chip } from '../ui.jsx'

export const STEPS = [
  { t: 'put() 只改内存', d: 'memtable 是 std::map<key, {ver, bytes}>：写入是 O(log n)、天然有序，封页时顺序就更好了。此时 mem_bytes 涨，盘上零改动。' },
  { t: 'commit() 开始：memtable → run', d: 'RunWriter 把有序记录一条条编码进页：超过 32 KB（BMAX）就换页；封页时先算 pfx 再做前缀压缩。整批记录变成一条不可变 run。' },
  { t: 'commit(false)：组提交，零 I/O', d: '异步模式下这批页直接进队列就返回，前台不等 I/O。攒到 async_flush_min（2048）条、或遇到 commit()、或工作区将满时才真正落盘 —— N 次逻辑提交摊成一次物理提交。' },
  { t: '页写：数据先于提交点', d: '后台线程顺序写页。此时超级块还是旧状态：掉电 → 回到“完整旧态”；绝不会出现“清单指向半页数据”。' },
  { t: '提交点：清单 + 超级块 + sync', d: 'wait_io() 等在飞页 → 写清单增量/快照 → 写超级块（双槽 + CRC）→ dev->sync()。到这一步才算持久化，返回 RC::OK。' },
  { t: '失败路径：只打脏标记，不阻塞', d: '页写失败或 NOSPACE ⇒ commit 返回错误并把 dirty_ 置位，绝不在失败路径里做 I/O；下一次任何公开调用先 ensure_ready()：等 I/O → 按盘重建 → 清脏。' },
]

export function SIDE({ step }) {
  return (
    <>
      <div className="panel-title">这一批的账</div>
      <div className="stat-grid">
        <div><b>7/8</b><span>用户可用容量</span></div>
        <div><b>2048</b><span>条/批（异步组提交）</span></div>
        <div><b>0</b><span>commit(false) 的 I/O</span></div>
        <div><b>1</b><span>后台写线程（可调）</span></div>
      </div>
      <div className="panel-title" style={{ marginTop: 14 }}>提交点的顺序（硬约束）</div>
      <ol className="ordered">
        <li>数据页写<span className="dim">（可异步，可失败）</span></li>
        <li>清单增量 / 快照页</li>
        <li>超级块（双槽 + CRC）</li>
        <li>dev-&gt;sync()</li>
      </ol>
      <Note tone="red">任何一步失败：账已脏 ⇒ 标记 dirty_，下次 ensure_ready() 重建。失败路径里绝不做 I/O —— 设备卡住时会把调用方一起拖死。</Note>
      {step === 5 && <Note tone="amber">这就是 r2 阶段 3 的注入点：每 7 次提交坏一笔写，提交必须报错、恢复后必须自愈。</Note>}
    </>
  )
}

const PHASES = [
  { t: 'mem', note: 'O(log n)' },
  { t: '封页', note: 'pfx + CRC' },
  { t: '入队', note: 'commit(false)' },
  { t: '落页', note: '顺序写' },
  { t: '提交点', note: '超级块 + sync' },
]

export default function Scene({ step }) {
  const on = (i) => step === i
  const pages = [0, 1, 2]
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />

      {/* memtable */}
      <Box x={30} y={60} w={250} h={210} tone="cyan" active={on(0)} dash={step > 1}>
        <text x={48} y={86} fontSize="13" fill="#7ff0ff" fontWeight="600">memtable</text>
        <text x={48} y={104} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">std::map · 有序 · 版本 ver</text>
        {[['1001', 'v7', '12 B'], ['1002', 'v3', '8 B'], ['1005', 'del', '0 B'], ['1009', 'v1', '20 B']].map((r, i) => (
          <g key={i} opacity={step === 0 ? 1 : Math.max(0.45, 1 - (step - 1) * 0.28)}>
            <rect x={46} y={116 + i * 26} width={218} height={20} rx={4} fill="rgba(56,225,255,.06)" stroke="rgba(56,225,255,.2)" />
            <text x={56} y={130 + i * 26} fontSize="10.5" fill="#9fd9ff" fontFamily="var(--mono)">key {r[0]}</text>
            <text x={140} y={130 + i * 26} fontSize="10.5" fill="#7ff0ff" fontFamily="var(--mono)">{r[1]}</text>
            <text x={210} y={130 + i * 26} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">{r[2]}</text>
          </g>
        ))}
        <text x={48} y={252} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">mem_bytes = 40</text>
      </Box>
      <Arrow d="M 284 165 L 330 165" tone="cyan" active={on(1)} />

      {/* RunWriter */}
      <Box x={334} y={60} w={260} h={210} tone="violet" active={on(1) || on(2)}>
        <text x={352} y={86} fontSize="13" fill="#c4b5fd" fontWeight="600">RunWriter</text>
        <text x={352} y={104} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">BMAX = 32 KB / 页 · 尾页不满则 pop</text>
        {pages.map((i) => {
          const active = step >= 1 && (step === 1 ? i <= 1 : true)
          return (
            <g key={i} opacity={active ? 1 : 0.3}>
              <rect x={352} y={116 + i * 44} width={224} height={34} rx={5}
                fill={active ? 'rgba(167,139,250,.16)' : 'rgba(148,163,184,.05)'}
                stroke={active ? '#c4b5fd' : 'rgba(148,163,184,.25)'} />
              <text x={362} y={138 + i * 44} fontSize="10.5" fill="#c4b5fd" fontFamily="var(--mono)">PAGE #{i}</text>
              <rect x={452} y={124 + i * 44} width={i === 2 ? 40 : 96} height={16} rx={3} fill="rgba(167,139,250,.45)" />
              <text x={566} y={138 + i * 44} textAnchor="end" fontSize="10" fill={i === 2 ? '#fde68a' : '#c4b5fd'} fontFamily="var(--mono)">
                {i === 2 ? 'tail' : 'used 1274'}
              </text>
            </g>
          )
        })}
      </Box>
      <Arrow d="M 598 165 L 644 165" tone="violet" active={on(2) || on(3)} />

      {/* 队列 + 后台线程 */}
      <Box x={648} y={60} w={322} h={210} tone="amber" active={on(2) || on(3)}>
        <text x={666} y={86} fontSize="13" fill="#fde68a" fontWeight="600">页写队列 / 后台线程</text>
        <text x={666} y={104} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">io_threads = 1 · inflight = 3</text>
        {[0, 1, 2].map((i) => (
          <g key={i} opacity={step >= 2 ? 1 : 0.25}>
            <rect x={666 + i * 100} y={118} width={86} height={44} rx={5}
              fill={step >= 3 && i < 2 ? 'rgba(62,224,139,.14)' : 'rgba(255,200,87,.10)'}
              stroke={step >= 3 && i < 2 ? 'rgba(62,224,139,.6)' : 'rgba(255,200,87,.45)'} />
            <text x={709 + i * 100} y={138} textAnchor="middle" fontSize="10" fill="#fde68a" fontFamily="var(--mono)">pid {40 + i}</text>
            <text x={709 + i * 100} y={152} textAnchor="middle" fontSize="9.5" fill="#8fa2c7" fontFamily="var(--mono)">
              {step >= 3 && i < 2 ? 'written' : 'inflight'}
            </text>
          </g>
        ))}
        <text x={666} y={186} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">worker: dev-&gt;write(pid, buf, len → 4 B 对齐)</text>
        <text x={666} y={204} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">写失败 ⇒ io_err_ 计数，提交点检查</text>
        <text x={666} y={228} fontSize="10.5" fill="#fda4af" fontFamily="var(--mono)">
          {step >= 3 ? '状态：2/3 已落盘' : '状态：等待调度'}
        </text>
        <text x={666} y={248} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">pending_free：提交点前不复用页号</text>
      </Box>

      {/* 时间轴 */}
      <Label x={30} y={310} size={12}>一次 commit 的内部顺序</Label>
      {PHASES.map((p, i) => {
        const x = 40 + i * 186
        const active = step === Math.min(i, 4) || (step >= 4 && i <= 4)
        return (
          <g key={p.t}>
            <rect x={x} y={326} width={168} height={56} rx={8}
              fill={on(i) ? 'rgba(56,225,255,.14)' : 'rgba(148,163,184,.05)'}
              stroke={on(i) ? '#7ff0ff' : 'rgba(148,163,184,.28)'} />
            <text x={x + 84} y={350} textAnchor="middle" fontSize="12" fill={on(i) ? '#7ff0ff' : '#c9d6f5'} fontFamily="var(--mono)">{p.t}</text>
            <text x={x + 84} y={368} textAnchor="middle" fontSize="10" fill="#8fa2c7" fontFamily="var(--mono)">{p.note}</text>
            {i < 4 && <Arrow d={`M ${x + 170} 354 L ${x + 184} 354`} tone="cyan" active={step >= i} arrow={false} dash="3 3" />}
          </g>
        )
      })}

      {/* 提交点示意 */}
      <g opacity={step >= 4 ? 1 : 0.35}>
        <rect x={40} y={404} width={920} height={76} rx={10} fill="rgba(62,224,139,.06)" stroke="rgba(62,224,139,.35)" />
        <text x={56} y={428} fontSize="12" fill="#86efac" fontFamily="var(--mono)">
          提交点 = 数据页全部落盘 + 清单落盘 + 超级块双槽 + sync()
        </text>
        <text x={56} y={448} fontSize="11" fill="#c9d6f5">
          超级块里记：vers_、man_first/man_last、nman、内联增量（≤32 条）、checkpoint 指针、空闲表根。
        </text>
        <text x={56} y={468} fontSize="11" fill="#8fa2c7">
          掉电只会落在“已提交的旧态”或“已提交的新态”之间 —— 因为数据页先写，且每页自带 CRC。
        </text>
      </g>

      {/* 失败路径 */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={40} y={496} width={920} height={76} rx={10} fill="rgba(255,93,115,.06)" stroke="rgba(255,93,115,.35)" />
        <text x={56} y={520} fontSize="12" fill="#fda4af" fontFamily="var(--mono)">
          失败：commit() 返回 RC::IO / NOSPACE / TIMEOUT，并置 dirty_ = true（不在这里 reload）
        </text>
        <text x={56} y={540} fontSize="11" fill="#c9d6f5">
          下一次 get/put/commit 入口先 ensure_ready()：wait_io() → reload() → 清脏。mem_ 与 pend_delta_ 原样保留，调用方重试即可。
        </text>
        <text x={56} y={560} fontSize="11" fill="#8fa2c7">
          「失败路径里不做 I/O」是有界性的关键：设备卡 3 秒时，失败也必须马上返回。
        </text>
      </g>
    </g>
  )
}
