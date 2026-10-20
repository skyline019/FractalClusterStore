import React from 'react'

/* ------------------------------------------------------------------ 基础 UI */

export function Chip({ children, tone = 'cyan', style }) {
  return <span className={`chip chip-${tone}`} style={style}>{children}</span>
}

export function KV({ k, v, note, tone }) {
  return (
    <div className="kv">
      <div className="kv-k">{k}</div>
      <div className={`kv-v ${tone ? 'tone-' + tone : ''}`}>{v}</div>
      {note && <div className="kv-note">{note}</div>}
    </div>
  )
}

export function Note({ children, tone = 'cyan' }) {
  return <div className={`note note-${tone}`}>{children}</div>
}

/* ------------------------------------------------------------------ SVG 元件 */

export function Defs() {
  return (
    <defs>
      <pattern id="grid" width="25" height="25" patternUnits="userSpaceOnUse">
        <path d="M 25 0 L 0 0 0 25" fill="none" stroke="rgba(120,160,255,.07)" strokeWidth="1" />
      </pattern>
      <marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
        <path d="M 0 0 L 10 5 L 0 10 z" fill="currentColor" />
      </marker>
      <filter id="glow" x="-60%" y="-60%" width="220%" height="220%">
        <feGaussianBlur stdDeviation="6" result="b" />
        <feMerge><feMergeNode in="b" /><feMergeNode in="SourceGraphic" /></feMerge>
      </filter>
      <linearGradient id="gCyan" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stopColor="#38e1ff" /><stop offset="1" stopColor="#3b82f6" />
      </linearGradient>
      <linearGradient id="gViolet" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stopColor="#a78bfa" /><stop offset="1" stopColor="#7c3aed" />
      </linearGradient>
      <linearGradient id="gGreen" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stopColor="#3ee08b" /><stop offset="1" stopColor="#10b981" />
      </linearGradient>
      <linearGradient id="gAmber" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stopColor="#ffc857" /><stop offset="1" stopColor="#f59e0b" />
      </linearGradient>
      <linearGradient id="gRed" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stopColor="#ff5d73" /><stop offset="1" stopColor="#e11d48" />
      </linearGradient>
    </defs>
  )
}

const TONE = {
  cyan: ['rgba(56,225,255,.10)', 'rgba(56,225,255,.55)', '#7ff0ff'],
  violet: ['rgba(167,139,250,.10)', 'rgba(167,139,250,.55)', '#c4b5fd'],
  green: ['rgba(62,224,139,.10)', 'rgba(62,224,139,.55)', '#86efac'],
  amber: ['rgba(255,200,87,.10)', 'rgba(255,200,87,.55)', '#fde68a'],
  red: ['rgba(255,93,115,.10)', 'rgba(255,93,115,.55)', '#fda4af'],
  slate: ['rgba(148,163,184,.07)', 'rgba(148,163,184,.35)', '#cbd5e1'],
}

export function Box({
  x, y, w, h, title, sub, tone = 'slate', active = false, dim = false,
  children, rx = 12, onClick, mono = false, dash = false,
}) {
  const [fill, stroke, text] = TONE[tone] || TONE.slate
  return (
    <g
      onClick={onClick}
      style={{ cursor: onClick ? 'pointer' : 'default', transition: 'opacity .35s' }}
      opacity={dim ? 0.28 : 1}
    >
      <rect
        x={x} y={y} width={w} height={h} rx={rx}
        fill={active ? fill.replace('.10', '.20') : fill}
        stroke={active ? text : stroke}
        strokeWidth={active ? 1.8 : 1}
        strokeDasharray={dash ? '6 5' : undefined}
        filter={active ? 'url(#glow)' : undefined}
      />
      {title && (
        <text x={x + w / 2} y={y + (sub ? h / 2 - 4 : h / 2 + 5)} textAnchor="middle"
          fill={active ? text : '#dbe4ff'} fontSize="15" fontWeight="600"
          fontFamily={mono ? 'var(--mono)' : undefined}>{title}</text>
      )}
      {sub && (
        <text x={x + w / 2} y={y + h / 2 + 15} textAnchor="middle" fill="#8fa2c7" fontSize="11.5">{sub}</text>
      )}
      {children}
    </g>
  )
}

export function Arrow({ d, tone = 'slate', active = false, width = 1.6, dash = '5 5', arrow = true }) {
  const [, , text] = TONE[tone] || TONE.slate
  return (
    <path
      d={d} fill="none" stroke={active ? text : 'rgba(148,163,184,.45)'}
      strokeWidth={active ? width + 0.8 : width}
      strokeDasharray={dash}
      markerEnd={arrow ? 'url(#ah)' : undefined}
      style={{ color: active ? text : 'rgba(148,163,184,.45)', transition: 'stroke .3s' }}
    />
  )
}

/** 沿路径移动的光点：p 为 0..1，用 SVG 的 path 长度近似做定位 */
export function Flow({ d, p = 0, tone = 'cyan', r = 4.5, show = true }) {
  const [, , text] = TONE[tone] || TONE.slate
  if (!show) return null
  return (
    <circle r={r} fill={text} filter="url(#glow)">
      <animateMotion dur="1s" repeatCount="indefinite" path={d} keyPoints={`${p};${Math.min(1, p + 0.02)}`} keyTimes="0;1" />
    </circle>
  )
}

export function Dot({ x, y, tone = 'cyan', r = 5, label }) {
  const [, , text] = TONE[tone] || TONE.slate
  return (
    <g>
      <circle cx={x} cy={y} r={r} fill={text} filter="url(#glow)" />
      {label && <text x={x} y={y - 10} textAnchor="middle" fill={text} fontSize="11">{label}</text>}
    </g>
  )
}

export function Label({ x, y, children, size = 12, tone = 'muted', anchor = 'start', weight = 400 }) {
  const color = tone === 'muted' ? '#8fa2c7' : tone === 'strong' ? '#e6ecff' : TONE[tone]?.[2] || tone
  return (
    <text x={x} y={y} fontSize={size} fill={color} textAnchor={anchor} fontWeight={weight}
      fontFamily={size <= 12 ? 'var(--mono)' : undefined}>{children}</text>
  )
}

/** 横向条形图（0..max），用于写放大/耗时对比 */
export function HBar({ x, y, w, h = 16, value, max, tone = 'cyan', label, right, delay = 0 }) {
  const [fill, stroke, text] = TONE[tone] || TONE.slate
  const bw = Math.max(2, (value / max) * w)
  return (
    <g>
      <rect x={x} y={y} width={w} height={h} rx={h / 2} fill="rgba(148,163,184,.08)" />
      <rect x={x} y={y} width={bw} height={h} rx={h / 2} fill={fill.replace('.10', '.75')}
        stroke={stroke} style={{ transition: 'width .7s cubic-bezier(.2,.8,.2,1)', transitionDelay: `${delay}ms` }} />
      {label && <text x={x - 10} y={y + h / 2 + 4} textAnchor="end" fontSize="12" fill="#c9d6f5" fontFamily="var(--mono)">{label}</text>}
      {right && <text x={x + w + 10} y={y + h / 2 + 4} fontSize="12" fill={text} fontFamily="var(--mono)">{right}</text>}
    </g>
  )
}
