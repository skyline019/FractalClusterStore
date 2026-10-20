import React from 'react'
import { BENCH3, BREAKDOWN, LIVE_BYTES, BENCH4, EVOLUTION, STATS } from '../data.js'

export const STEPS = []
export const KIND = 'html'

export function SIDE() {
  return (
    <>
      <div className="panel-title">口径</div>
      <ul className="bullets">
        <li>写放大 = 设备写入字节 / 逻辑值字节</li>
        <li>bench3：随机键，值 4–28 B，RamDev 8 192 页</li>
        <li>bench4：50 000 键 / 32 B 值 / 每 500 次提交 / 区间 100 行 × 2 000 次</li>
        <li>SQLite：WAL + synchronous=FULL + cache 16 MB</li>
      </ul>
      <div className="panel-title" style={{ marginTop: 14 }}>这一轮改了什么</div>
      <ul className="bullets">
        <li>分层合并门槛 20% → 30%</li>
        <li>清单快照页只写 MHDR + n×40 字节</li>
        <li>run 级存活账改成段级精确记账</li>
        <li>新增 f6：每次提交都 verify</li>
      </ul>
    </>
  )
}

const maxWa = Math.max(...BENCH3.map((b) => b.wa))

export default function Scene() {
  return (
    <div className="bench">
      <div className="bench-row">
        <div className="card grow">
          <div className="card-h">bench3 · 写放大 <span className="dim">（越小越好）</span></div>
          <table className="tbl">
            <thead>
              <tr><th>ops / commit</th><th>写放大</th><th></th><th>probes/get</th><th>段数峰值</th></tr>
            </thead>
            <tbody>
              {BENCH3.map((b) => (
                <tr key={b.ops}>
                  <td className="mono">{b.ops}</td>
                  <td className="mono strong">{b.wa.toFixed(2)}×</td>
                  <td className="bar-cell">
                    <div className="tbar"><i style={{ width: `${(b.wa / maxWa) * 100}%` }} /></div>
                  </td>
                  <td className="mono">{b.probes.toFixed(2)}</td>
                  <td className="mono">{b.seg}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="card-note">200k/2000 那一档：读页 81 245 → 50 346，耗时 0.95 s → 0.36 s。</div>
        </div>

        <div className="card">
          <div className="card-h">写放大分解 <span className="dim">200k/2000</span></div>
          <div className="stack">
            {BREAKDOWN.map((b) => (
              <i key={b.name} className={`seg seg-${b.tone}`} style={{ width: `${(b.bytes / (LIVE_BYTES * 2.57)) * 100}%` }}>
                <span>{b.name} {b.bytes}</span>
              </i>
            ))}
          </div>
          <div className="legend">
            {BREAKDOWN.map((b) => <span key={b.name}><i className={`dot dot-${b.tone}`} />{b.name} {b.bytes} MB</span>)}
            <span><i className="dot dot-green" />活数据 {LIVE_BYTES} MB</span>
          </div>
          <div className="card-note">合计 7.98 MB ÷ 3.10 MB = 2.57×。</div>
        </div>
      </div>

      <div className="bench-row">
        <div className="card grow">
          <div className="card-h">同一负载的演进 <span className="dim">（200k / 2000）</span></div>
          <div className="evo">
            {EVOLUTION.map((e) => (
              <div className="evo-row" key={e.label}>
                <span className="evo-label">{e.label}</span>
                <div className="evo-track">
                  <i className={`evo-fill f-${e.tone}`} style={{ width: `${(Math.log10(e.sec + 1) / Math.log10(17.3)) * 100}%` }} />
                  <span className="evo-val">{e.sec} s · {e.reads.toLocaleString()} 读页</span>
                </div>
              </div>
            ))}
          </div>
          <div className="card-note">对数刻度；三次都是同一份代码的不同版本、同一台机器。</div>
        </div>

        <div className="card">
          <div className="card-h">bench4 · µs/op vs SQLite</div>
          <table className="tbl">
            <thead><tr><th>阶段</th><th>同步</th><th>异步+整理</th><th>SQLite</th></tr></thead>
            <tbody>
              {BENCH4.map((b) => (
                <tr key={b.phase}>
                  <td>{b.phase}</td>
                  <td className="mono">{b.sync.toFixed(2)}</td>
                  <td className="mono strong">{b.async.toFixed(2)}</td>
                  <td className="mono dim2">{b.sqlite.toFixed(1)}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="card-note">文件 2.10 MB vs 2.35 MB；每记录 41.9 B vs 46.9 B。</div>
        </div>
      </div>

      <div className="bench-row">
        <div className="card grow">
          <div className="card-h">测试</div>
          <div className="tests">
            {['t1..t6', 'tchk', 'x1', 'x2', 'f1 语义对拍', 'f2 快照/回退', 'f3 NOR/掉电', 'f4 规模/重开', 'f5 回收/收缩', 'f6 存活账', 'q1 查询/游标', 'p1 前缀压缩', 'r2 事故注入', 's2 长跑不漂移'].map((t) => (
              <span className="tpill" key={t}><i className="ok" />{t}</span>
            ))}
          </div>
          <div className="card-note">共 19 个可执行项全绿；`make asan` 无 ASan/UBSan 报错。</div>
        </div>
        <div className="card">
          <div className="card-h">四个数字</div>
          <div className="kpis">
            {STATS.map((s) => (
              <div key={s.k}><b>{s.v}</b><span>{s.k}</span><em>{s.note}</em></div>
            ))}
          </div>
        </div>
      </div>
    </div>
  )
}
