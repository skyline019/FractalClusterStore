import React from 'react'
import { Box, Arrow, Label, Dot, Defs, Chip } from '../ui.jsx'

export const STEPS = [
  { t: 'put()：只写内存', d: '记录进 memtable（有序 map，O(log n)），此刻一个字节都还没落盘。返回 RC::OK 不等于持久化 —— 持久化由 commit() 定义。' },
  { t: 'commit()：先把 memtable 封成 run', d: 'RunWriter 按 32 KB 切块、逐页编码（页内前缀压缩），页尾没写满就不占页。这一批数据从此变成不可变的 run。' },
  { t: '页写：顺序落盘', d: '页交给设备写。异步模式下一批页直接进队列，由 io_threads 个后台线程顺序写；前台不等 I/O（commit(false) 是零 I/O 的）。' },
  { t: '提交点：超级块才算数', d: '数据页全部落盘之后，才写超级块（双槽 + CRC）。顺序是硬约束：数据先于提交点，所以掉电只会回到“完整旧态”。' },
  { t: '清单：把 run 挂到树上', d: '清单页记录 run 的 (page0, npages, seq, ver, lvl, first, last)；超级块里还能内联 32 条增量，链长超过 64 就写全量快照。' },
  { t: 'run → 段 → 单元', d: 'run 按键域挂到各个 cell 的段表里，段身份 (seq, page0)。读的时候按这个全序从新到旧扫。' },
  { t: '后台维护：合并 / 压实 / 回收', d: '分层合并控读放大、单元压实回收垃圾、整条 run 全死则零写摘链。全部是机会性的：拿不到空间就跳过，绝不把用户提交搞失败。' },
]

export function SIDE() {
  return (
    <>
      <div className="panel-title">一次写入的七个站</div>
      <div className="mini-list">
        {STEPS.map((s, i) => (
          <div className="mini-row" key={i}>
            <span className="mini-no">{String(i + 1).padStart(2, '0')}</span>
            <span className="mini-t">{s.t.replace(/[：:].*$/, '')}</span>
          </div>
        ))}
      </div>
      <div className="panel-title" style={{ marginTop: 16 }}>这条链上的硬约束</div>
      <ul className="bullets">
        <li><b>数据先于提交点</b>：页没写完，超级块不许动</li>
        <li><b>页号会复用</b>：段身份必须带 seq</li>
        <li><b>维护机会性</b>：NOSPACE 只跳过本轮</li>
      </ul>
    </>
  )
}

const NODES = [
  { x: 30, y: 92, w: 150, h: 64, title: 'put()', sub: '应用写入', tone: 'cyan' },
  { x: 220, y: 92, w: 150, h: 64, title: 'memtable', sub: '有序 map · 内存', tone: 'cyan' },
  { x: 410, y: 92, w: 160, h: 64, title: 'RunWriter', sub: '封页 · 前缀压缩', tone: 'violet' },
  { x: 610, y: 92, w: 160, h: 64, title: '页写队列', sub: 'io_threads=1', tone: 'violet' },
  { x: 810, y: 92, w: 160, h: 64, title: '提交点', sub: '超级块 + CRC', tone: 'amber' },
]

const NODES2 = [
  { x: 810, y: 214, w: 160, h: 64, title: '清单链', sub: '102 条/页', tone: 'amber' },
  { x: 610, y: 214, w: 160, h: 64, title: 'run', sub: '不可变 · seq 单调', tone: 'green' },
  { x: 410, y: 214, w: 160, h: 64, title: '段 (seg)', sub: '(seq, rid) 全序', tone: 'green' },
  { x: 220, y: 214, w: 150, h: 64, title: '单元 (cell)', sub: '键域 + 段集合', tone: 'green' },
  { x: 30, y: 214, w: 150, h: 64, title: '盘', sub: '4 KB 页', tone: 'green' },
]

export default function Scene({ step }) {
  const a1 = step === 0, a2 = step === 1, a3 = step === 2, a4 = step === 3, a5 = step === 4, a6 = step === 5, a7 = step === 6
  return (
    <g>
      <Defs />
      <rect x="0" y="0" width="1000" height="600" fill="url(#grid)" opacity="0.7" />
      <Label x={30} y={44} size={13}>写入方向 →</Label>

      {NODES.map((n, i) => (
        <Box key={n.title} {...n} active={(i === 0 && a1) || (i === 1 && a1) || (i === 2 && a2) || (i === 3 && a3) || (i === 4 && a4)} />
      ))}
      <Arrow d="M 185 124 L 214 124" tone="cyan" active={a1} />
      <Arrow d="M 375 124 L 404 124" tone="cyan" active={a1 || a2} />
      <Arrow d="M 575 124 L 604 124" tone="violet" active={a2 || a3} />
      <Arrow d="M 775 124 L 804 124" tone="violet" active={a3 || a4} />

      {/* 提交点往下走到清单 → run → 段 → 单元 → 盘 */}
      <Arrow d="M 890 160 L 890 208" tone="amber" active={a5} />
      <Arrow d="M 805 246 L 776 246" tone="amber" active={a5 || a6} />
      <Arrow d="M 605 246 L 576 246" tone="green" active={a6} />
      <Arrow d="M 405 246 L 376 246" tone="green" active={a6} />
      <Arrow d="M 215 246 L 186 246" tone="green" active={a6 || a7} />
      {NODES2.map((n) => (
        <Box key={n.title} {...n} active={(n.title === '清单链' && a5) || (['run', '段 (seg)', '单元 (cell)'].includes(n.title) && a6) || (n.title === '盘' && a7)} />
      ))}

      {/* 底色注解 */}
      <rect x="30" y="316" width="940" height="1" fill="rgba(148,163,184,.18)" />
      <Label x={30} y={348} size={12} tone="muted">后台维护（机会性：拿不到空间就跳过本轮，绝不让用户提交失败）</Label>

      <Box x={30} y={368} w={296} h={182} tone="cyan" active={a7} rx={14}>
        <text x={48} y={396} fontSize="13.5" fill="#7ff0ff" fontWeight="600">读路径</text>
        <text x={48} y={422} fontSize="11.5" fill="#a9b8d8">Bloom(64 位·4 探位·0.24%)</text>
        <text x={48} y={442} fontSize="11.5" fill="#a9b8d8">→ 段 (seq,rid) 新→旧</text>
        <text x={48} y={462} fontSize="11.5" fill="#a9b8d8">→ 页 fence 二分 → 前缀还原</text>
        <text x={48} y={492} fontSize="11.5" fill="#8fa2c7">probes/get 0.12 ~ 0.65</text>
        <text x={48} y={512} fontSize="11.5" fill="#8fa2c7">每页一次 CRC32C（硬件 7.5 GB/s）</text>
      </Box>

      <Box x={352} y={368} w={296} h={182} tone="violet" active={a7} rx={14}>
        <text x={370} y={396} fontSize="13.5" fill="#c4b5fd" fontWeight="600">合并 / 压实</text>
        <text x={370} y={422} fontSize="11.5" fill="#a9b8d8">每层 ≤ 8 条 run，超出升层</text>
        <text x={370} y={442} fontSize="11.5" fill="#a9b8d8">垃圾 ≥ 30% 才重写这一层</text>
        <text x={370} y={462} fontSize="11.5" fill="#a9b8d8">单元级：dead ≥ 2×live，预算 4~64 页</text>
        <text x={370} y={492} fontSize="11.5" fill="#8fa2c7">整条 run 全死 ⇒ 零写摘链</text>
        <text x={370} y={512} fontSize="11.5" fill="#8fa2c7">产物 seq = 源里最大的那个</text>
      </Box>

      <Box x={674} y={368} w={296} h={182} tone="amber" active={a7} rx={14}>
        <text x={692} y={396} fontSize="13.5" fill="#fde68a" fontWeight="600">空间 / 不挂死</text>
        <text x={692} y={422} fontSize="11.5" fill="#a9b8d8">pending_free：提交点前不复用页</text>
        <text x={692} y={442} fontSize="11.5" fill="#a9b8d8">deferred：有快照时延迟归还</text>
        <text x={692} y={462} fontSize="11.5" fill="#a9b8d8">compact_file：归一两次 + shrink</text>
        <text x={692} y={492} fontSize="11.5" fill="#8fa2c7">io_timeout_ms：等待有界，到点 TIMEOUT</text>
        <text x={692} y={512} fontSize="11.5" fill="#8fa2c7">失败只打脏标记，下次调用 ensure_ready 自愈</text>
      </Box>

      {/* 流动光点：沿主链走 */}
      <path id="flow1" d="M 105 124 L 890 124" fill="none" stroke="none" />
      <circle r="3.5" fill="#7ff0ff" opacity={step <= 3 ? 1 : 0}>
        <animateMotion dur="2.6s" repeatCount="indefinite" keyPoints="0;1" keyTimes="0;1" calcMode="linear">
          <mpath href="#flow1" />
        </animateMotion>
      </circle>
      <path id="flow2" d="M 890 124 L 890 246 L 105 246" fill="none" stroke="none" />
      <circle r="3.5" fill="#86efac" opacity={step >= 4 ? 1 : 0}>
        <animateMotion dur="3.2s" repeatCount="indefinite" keyPoints="0;1" keyTimes="0;1" calcMode="linear">
          <mpath href="#flow2" />
        </animateMotion>
      </circle>
    </g>
  )
}
