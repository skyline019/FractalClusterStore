import React, { useState } from 'react'
import { PITS, CATS } from '../data.js'

const HOT = new Set([30, 34, 35, 36, 20, 29])

export const STEPS = []
export const KIND = 'html'

export function SIDE() {
  return (
    <>
      <div className="panel-title">怎么读这面墙</div>
      <p className="mini-p">36 条全部来自真实调试：每条都写了一句话的“现象”和一句话的“修法”，并且最后都落成了一个不变量或一条测试。</p>
      <div className="panel-title" style={{ marginTop: 14 }}>最贵的三条</div>
      <ul className="bullets">
        <li><b>#30 Bloom 截成 32 位</b>：不报错，只是慢 17×</li>
        <li><b>#20 切分没收紧页区间</b>：O(n²)，30k 全扫 698 → 68 ns/条</li>
        <li><b>#29 页指针跨 page()</b>：随机坏清单项，追了一整轮</li>
      </ul>
      <div className="panel-title" style={{ marginTop: 14 }}>本轮新增</div>
      <ul className="bullets">
        <li>#34 合并产物的存活账要用自己的段身份</li>
        <li>#35 切分时段的计数要退回</li>
        <li>#36 压实产物的 seq 要在删源之前取</li>
      </ul>
    </>
  )
}

export default function Scene() {
  const [cat, setCat] = useState('全部')
  const [open, setOpen] = useState(34)
  const list = PITS.filter((p) => cat === '全部' || p.cat === cat)
  return (
    <div className="pits">
      <div className="pits-bar">
        {CATS.map((c) => {
          const n = c === '全部' ? PITS.length : PITS.filter((p) => p.cat === c).length
          return (
            <button key={c} className={`pills ${cat === c ? 'on' : ''}`} onClick={() => setCat(c)}>
              {c}<span className="pills-n">{n}</span>
            </button>
          )
        })}
        <span className="pits-tip">点任意一条展开「现象 → 修法」</span>
      </div>
      <div className="pits-grid">
        {list.map((p) => (
          <div
            key={p.id}
            className={`pit ${open === p.id ? 'open' : ''} ${HOT.has(p.id) ? 'hot' : ''}`}
            onClick={() => setOpen(open === p.id ? null : p.id)}
          >
            <div className="pit-top">
              <span className="pit-no">#{String(p.id).padStart(2, '0')}</span>
              <span className={`pit-cat cat-${p.cat.replace('/', '')}`}>{p.cat}</span>
              {HOT.has(p.id) && <span className="pit-flag">坑王</span>}
            </div>
            <div className="pit-title">{p.title}</div>
            {open === p.id && (
              <div className="pit-body">
                <div className="pit-row"><span className="pit-k">现象</span><span>{p.body}</span></div>
                <div className="pit-row"><span className="pit-k fix">修法</span><span>{p.fix}</span></div>
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  )
}
