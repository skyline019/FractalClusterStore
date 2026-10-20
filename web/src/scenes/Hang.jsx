import React from 'react'
import { Defs, Label, Note, HBar } from '../ui.jsx'

export const STEPS = [
  { t: '设备卡住的时候，默认行为是无限等', d: '提交点必须等所有在飞的页写完。设备一次写要 3 秒，40 页就是 120 秒 —— 调用方看起来就是“挂死了”。' },
  { t: 'set_io_timeout_ms(150)：每次等待都有截止时间', d: 'wait_io() 变成 wait_until(deadline)。到点 ⇒ ++io_timeout_、置 io_stalled_、返回 false；commit 立刻返回 RC::TIMEOUT（实测 150 ms，不是 12 000 ms）。' },
  { t: 'io_stalled_：一次超时之后全部快失败', d: '如果每页各等 150 ms，40 页还是 6 秒。停滞标记一置位，后续所有有界等待直接失败 —— 整个提交的代价就从“页数 × 超时”变成“一次超时”。' },
  { t: '失败不阻塞：只打脏标记', d: 'commit 的 fail() 里绝不做 reload（那是 I/O）。只置 dirty_=true 返回；mem_ 和 pend_delta_ 原样保留，调用方重试即可。' },
  { t: '停滞怎么解除？队列排空', d: 'worker 循环末尾和 wait_io 的自排空分支都检查：inflight_==0 ⇒ io_stalled_=false。设备缓过来之后，下一次调用自然恢复。' },
  { t: 'ensure_ready：下次调用先自愈', d: 'get/put/del/commit/consolidate/compact_file/verify/Cursor::open 的入口都先 ensure_ready()：wait_io → reload → 清脏。恢复后提交 OK，verify 0 错。' },
]

export function SIDE() {
  return (
    <>
      <div className="panel-title">r2 阶段 5 的实测</div>
      <div className="code">SlowDev::do_write: sleep(3000ms)
set_io_timeout_ms(150)

卡住时的提交：TIMEOUT（150 ms）
超时计数：41
恢复后提交：OK
verify：0 错</div>
      <div className="panel-title" style={{ marginTop: 14 }}>为什么不是“每页 150 ms”</div>
      <p className="mini-p">40 页 × 150 ms = 6 s，比不设超时还难受。停滞标记把“页数 × 超时”压成“一次超时”。</p>
      <Note tone="amber">停滞快失败只在用户显式开了超时（io_timeout_ms ≠ 0）时生效。默认路径不能被污染，否则恢复后第一次提交还是 TIMEOUT。</Note>
      <Note tone="green" style={{ marginTop: 8 }}>队列排空（inflight_ == 0）必须清停滞 —— 两个地方都要写：worker 循环末尾、wait_io 自排空分支。</Note>
    </>
  )
}

export default function Scene({ step }) {
  const stalled = step >= 2
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.65" />
      <Label x={30} y={26} size={13}>40 个在飞的页 · 设备一次写 3 s</Label>

      {/* 页栅格 */}
      {Array.from({ length: 40 }).map((_, i) => {
        const x = 30 + (i % 20) * 47
        const y = 52 + Math.floor(i / 20) * 40
        const stuck = i === 0
        const fast = stalled && i > 0
        return (
          <g key={i}>
            <rect x={x} y={y} width={42} height={30} rx={4}
              fill={stuck ? 'rgba(255,93,115,.22)' : fast ? 'rgba(255,200,87,.10)' : 'rgba(148,163,184,.06)'}
              stroke={stuck ? 'rgba(255,93,115,.7)' : fast ? 'rgba(255,200,87,.35)' : 'rgba(148,163,184,.22)'} />
            <text x={x + 21} y={y + 20} textAnchor="middle" fontSize="9" fill="#8fa2c7" fontFamily="var(--mono)">{i}</text>
            {stuck && step >= 1 && (
              <text x={x + 21} y={y + 20} textAnchor="middle" fontSize="9" fill="#0b1020" fontFamily="var(--mono)" fontWeight="700">3s</text>
            )}
          </g>
        )
      })}
      <text x={30} y={146} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
        第 0 页写入卡住；其余 39 页在它后面排队（单队列 + 1 个 worker）
      </text>

      {/* 时间对比 */}
      <Label x={30} y={176} size={12}>提交从调用到返回的墙钟时间</Label>
      <HBar x={170} y={190} w={690} h={18} value={120} max={120} tone="red" label="无超时" right="120 s" />
      <HBar x={170} y={218} w={690} h={18} value={6} max={120} tone="amber" label="每页 150 ms" right="6 s" />
      <HBar x={170} y={246} w={690} h={18} value={0.15} max={120} tone="green" label="停滞快失败" right="150 ms" delay={150} />
      <text x={170} y={284} fontSize="10.5" fill="#86efac" fontFamily="var(--mono)">
        41 次超时计数全部发生在第一个 150 ms 窗口内 —— 之后是 O(1) 的快失败
      </text>

      {/* 状态机 */}
      <g opacity={step >= 2 ? 1 : 0.25}>
        <Label x={30} y={316} size={12}>停滞标记的状态机</Label>
        {[
          ['正常', '等待有截止时间', '#86efac'],
          ['超时一次', 'io_timeout_++ / io_stalled_=true', '#ffc857'],
          ['快失败', '后续等待立即返回 false', '#ff5d73'],
          ['队列排空', 'inflight_==0 ⇒ io_stalled_=false', '#38e1ff'],
          ['自愈', 'ensure_ready: wait_io→reload→清脏', '#3ee08b'],
        ].map((r, i) => (
          <g key={i}>
            <rect x={30 + i * 190} y={330} width={176} height={66} rx={8}
              fill={`${r[2]}14`} stroke={`${r[2]}66`} />
            <text x={118 + i * 190} y={354} textAnchor="middle" fontSize="11.5" fill={r[2]} fontFamily="var(--mono)">{r[0]}</text>
            <text x={118 + i * 190} y={374} textAnchor="middle" fontSize="9.5" fill="#8fa2c7">{r[1]}</text>
            {i < 4 && <path d={`M ${30 + i * 190 + 178} 363 L ${30 + (i + 1) * 190 - 2} 363`} stroke="rgba(148,163,184,.45)" strokeWidth="1.4" markerEnd="url(#ah)" />}
          </g>
        ))}
      </g>

      {/* ensure_ready */}
      <g opacity={step >= 5 ? 1 : 0.25}>
        <rect x={30} y={436} width={940} height={92} rx={9} fill="rgba(62,224,139,.05)" stroke="rgba(62,224,139,.3)" />
        <text x={46} y={460} fontSize="12" fill="#86efac" fontFamily="var(--mono)">ensure_ready()：所有公开入口的第一件事</text>
        <text x={46} y={480} fontSize="10.5" fill="#c9d6f5" fontFamily="var(--mono)">
          if (!dirty_) return OK;  if (!wait_io()) return TIMEOUT;  reload();  dirty_ = false;
        </text>
        <text x={46} y={500} fontSize="10.5" fill="#8fa2c7">
          失败路径因此永远有界：commit 里 fail() 只打标记；真正的 I/O（reload）推迟到下一次调用。
        </text>
        <text x={46} y={518} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          r2 断言：卡住时提交 150 ms 返回 TIMEOUT → 恢复后提交 OK → verify 0 错
        </text>
      </g>

      {/* 其余有界等待点 */}
      <g opacity={step >= 1 ? 1 : 0.3}>
        <text x={30} y={556} fontSize="10.5" fill="#8fa2c7" fontFamily="var(--mono)">
          其余有界点：commit 的 4 处 wait_io、remove/flush、compact_file、Cursor::open；stop_workers/reload 故意用无界等待（换进程时才允许）。
        </text>
      </g>
    </g>
  )
}
