import React, { useEffect, useRef, useState } from 'react'
import { SCENES, STATS } from './data.js'
import * as Overview from './scenes/Overview.jsx'
import * as Page from './scenes/Page.jsx'
import * as Write from './scenes/Write.jsx'
import * as Manifest from './scenes/Manifest.jsx'
import * as Read from './scenes/Read.jsx'
import * as Compact from './scenes/Compact.jsx'
import * as Space from './scenes/Space.jsx'
import * as Crash from './scenes/Crash.jsx'
import * as Hang from './scenes/Hang.jsx'
import * as Pits from './scenes/Pits.jsx'
import * as Bench from './scenes/Bench.jsx'
import * as Format from './scenes/Format.jsx'

const REG = { overview: Overview, page: Page, write: Write, manifest: Manifest, read: Read, compact: Compact, space: Space, crash: Crash, hang: Hang, pits: Pits, format: Format, data: Bench }

export default function App() {
  const [si, setSi] = useState(0)
  const [step, setStep] = useState(0)
  const [playing, setPlaying] = useState(true)
  const [speed, setSpeed] = useState(1)
  const scene = SCENES[si]
  const mod = REG[scene.id]
  const isSvg = mod.KIND !== 'html'
  const steps = mod.STEPS || []
  const last = Math.max(0, steps.length - 1)
  const timer = useRef(null)

  useEffect(() => {
    if (!isSvg || !playing) return
    timer.current = setTimeout(() => {
      setStep((s) => (s < last ? s + 1 : s))
    }, 4200 / speed)
    return () => clearTimeout(timer.current)
  }, [step, playing, speed, isSvg, last])

  useEffect(() => {
    const onKey = (e) => {
      if (e.key === ' ') { e.preventDefault(); setPlaying((p) => !p) }
      if (e.key === 'ArrowRight') { e.preventDefault(); setStep((s) => Math.min(last, s + 1)) }
      if (e.key === 'ArrowLeft') { e.preventDefault(); setStep((s) => Math.max(0, s - 1)) }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [last])

  const goto = (i) => { setSi(i); setStep(0); setPlaying(true) }
  const Scene = mod.default
  const cur = steps[step]

  return (
    <div className="app">
      <header className="top">
        <div className="brand">
          <span className="logo">FCS</span>
          <div>
            <div className="brand-t">存储引擎 · 运行时解剖</div>
            <div className="brand-s">页 → run → 段 → 单元 · 一份能跑的数据结构</div>
          </div>
        </div>
        <div className="chips">
          {STATS.map((s) => (
            <div className="chip-stat" key={s.k}>
              <b>{s.v}</b>
              <span>{s.k}</span>
              <em>{s.note}</em>
            </div>
          ))}
        </div>
      </header>

      <div className="body">
        <nav className="chapters">
          {SCENES.map((s, i) => (
            <button key={s.id} className={`chap ${i === si ? 'on' : ''}`} onClick={() => goto(i)}>
              <span className="chap-no">{s.no}</span>
              <span className="chap-t">{s.title}</span>
              <span className="chap-s">{s.sub}</span>
            </button>
          ))}
          <div className="chap-foot">
            <div className="hint">空格 播放/暂停 · ← → 单步</div>
          </div>
        </nav>

        <main className={`stage ${isSvg ? '' : 'html-stage'}`}>
          <div className="stage-head">
            <div className="sh-l">
              <span className="sh-no">{scene.no}</span>
              <span className="sh-title">{scene.title}</span>
              <span className="sh-sub">{scene.sub}</span>
            </div>
            {isSvg && (
              <div className="sh-r">
                <span className="sh-step">{step + 1}/{steps.length}</span>
                <button className="btn ghost" onClick={() => setPlaying((p) => !p)}>{playing ? '暂停' : '播放'}</button>
                <button className="btn ghost" onClick={() => setSpeed((s) => (s === 1 ? 2 : s === 2 ? 0.5 : 1))}>{speed}×</button>
              </div>
            )}
          </div>

          {isSvg ? (
            <>
              <div className="canvas">
                <svg viewBox="0 0 1000 600" preserveAspectRatio="xMidYMid meet">
                  <Scene step={step} />
                </svg>
              </div>
              <div className="caption">
                <div className="cap-t">{cur?.t}</div>
                <div className="cap-d">{cur?.d}</div>
                <div className="cap-dots">
                  {steps.map((_, i) => (
                    <button key={i} className={`dot ${i === step ? 'on' : ''} ${i < step ? 'past' : ''}`} onClick={() => { setStep(i); setPlaying(false) }} />
                  ))}
                </div>
              </div>
            </>
          ) : (
            <div className="html-scroll"><Scene step={step} /></div>
          )}
        </main>

        <aside className="side">
          <mod.SIDE step={step} />
        </aside>
      </div>
    </div>
  )
}
