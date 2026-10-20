# FCS 存储引擎 · 运行时解剖（React 动画页）

把 `../include/fcs.h`（约 3 000 行）里的存储引擎拆成 11 个可以单步、自动播放的动态章节：
页格式 → 写入/提交 → 清单与超级块 → 读路径 → 合并压实 → 空间回收 → 崩溃恢复 → 不挂死 →
36 个坑 → **真字节** → 实测数据。

## 跑起来

```bash
npm install
npm run dev       # http://localhost:5173 （已绑 0.0.0.0）
npm run build     # 产出 dist/index.html —— 单文件，双击就能开
```

`npm run build` 会把 React、CSS、以及 `src/realpage.json` 全部内联成一个
**约 260 KB 的独立 HTML**（`../fcs-anim.html` 就是构建产物）。没有外部请求，
断网、在沙箱 iframe 里都能跑。

## 交互

| 操作 | 作用 |
|---|---|
| 左侧章节 | 切换场景（进入时自动播放） |
| `空格` | 播放 / 暂停 |
| `←` `→` | 单步前进 / 后退 |
| 1× / 2× / 0.5× | 播放速度 |
| 圆点 | 直接跳到某一步 |
| 「36 个坑」页 | 按类别筛选，点卡片展开「现象 → 修法」 |

## 目录

```
src/
  App.jsx              壳：章节导航 / 播放控制 / 键盘 / 侧栏
  ui.jsx               SVG 元件：Box / Arrow / Label / HBar / Defs
  data.js              所有事实数据：常量、36 个坑、bench3/bench4、测试清单
  realpage.json        真字节：由 web/dump.cpp 从 RamDev 里 dump 出来的
                       超级块两槽 + 清单页 + 最新数据页（十六进制 + 解出的字段）
  scenes/
    Overview.jsx       00 全景：一次 put 到盘上的七站
    Page.jsx           01 页的解剖：40 B 页头 / 10 B 记录头 / pfx 前缀压缩 / CRC
    Write.jsx          02 写入与提交：memtable → RunWriter → 队列 → 提交点 → 失败路径
    Manifest.jsx       03 清单与超级块：双槽 / 内联增量 / 链式快照 / 挂载重放
    Read.jsx           04 读路径：Bloom → 段 (seq,rid) → fence 二分 → 前缀还原
    Compact.jsx        05 合并与压实：LSM 分层 / 垃圾门槛 30% / 写放大分解
    Space.jsx          06 空间回收：pending_free / deferred / 归一两次 / shrink / 7-8 容量
    Crash.jsx          07 崩溃恢复：掉电注入点 / 双槽择优 / NOR / CRC
    Hang.jsx           08 绝不挂死：有界等待 / 停滞快失败 / 失败不阻塞 / 自愈
    Pits.jsx           09 36 个坑（可筛选、可展开）
    Format.jsx         10 真字节：十六进制 × 字段对号
    Bench.jsx          11 实测数据：bench3 / bench4 / 19 项测试
```

## 「真字节」怎么来的

`dump.cpp` 就是一个普通测试程序：写 10 条 → 覆盖 3 条 → 删 1 条 → 再提交 40 次
（撑爆超级块里 32 条的内联增量，逼出真正的清单页），然后把设备里的原始字节
按 JSON 打出来。

```bash
g++ -std=c++17 -O2 -pthread -I../include dump.cpp -o dump
./dump > src/realpage.json
```

页里的每个字段（kind / chk / ver / first / last / nrec / used / pfx / fmt）都能在
第 10 章里和十六进制逐字节对上；`used_pages=38 / cells=1 / runs=11 / recs=130 /
live=1564 B / errors=0` 这些数也来自同一次运行的 `verify()`。

## 数据来源

所有数字来自仓库的 `docs/10-design/DESIGN3.md` §9/§10/§11 与 `make test` / `bench3` / `bench4`
的最近一次实测（2026-10-19）：

- 写放大 2.57×、0.36 s、50 346 读页（200 000 / 2 000 档）
- 分层合并门槛 `FCS_LVL_DIRTY_PCT` 20% → 30% 的对比
- 写放大分解：flush 4.08 MB + merge 3.32 MB + manifest 0.58 MB
- bench4 四档 vs SQLite；文件 2.10 MB vs 2.35 MB
- 19 个测试项、卡住设备时 150 ms 返回 `RC::TIMEOUT`

---

## 第二个页面：`fcs-pits.html`（坑库 · 老/新示例数据动画 · 深析 · 含 R15–R18）

和上面的 React 页是兄弟关系：上面讲**引擎怎么运转**，这个页逐坑讲**哪里错了、为什么、怎么修的**。
纯 HTML/JS 单文件、无构建依赖、无外部请求（沙箱 iframe / 双击都能开）。

```
web/pits/
  template.html          壳 + 样式 + 播放引擎（故事板渲染、FLIP 过渡、键盘、搜索、hash 路由）
  build.py               把 template + data/ 拼成 ../../fcs-pits.html
  data/pits_text.json    由 docs/10-design/DESIGN3.md §12 自动抽出的 108 条文本（坑 1~109，文档跳号 58）
  data/batch1a.js        第 1 批 (a)：坑 92 ~ 101 的故事板 + 代码对比 + 分节讲解
  data/batch1b.js        第 1 批 (b)：坑 102 ~ 109 + 第 14 轮 7 条"假象 / 设计性发现"（R14-1 … R14-7）
  data/batch2a.js        第 2 批 (a)：坑 1 ~ 18（v1 / v2 基础：段与单元、seq 全序、清单身份、两阶段释放、NOR、账）
  data/batch2b.js        第 2 批 (b)：坑 19 ~ 36（异步写、retighten、硬件 CRC、容量、满库、回退、Bloom、存活账）
  data/batch3a.js        第 3 批 (a)：坑 37 ~ 52（容量 PageId、溢出链 A2、快照判据、链接管 take_set_/loc_ready）
  data/batch3b.js        第 3 批 (b)：坑 53 ~ 68（链所有权、L4 范围墓碑 maxhi/run_fully_hidden、L2 ghost、M1、g4 预取、s2 入账顺序）
  data/batch3c.js        第 3 批 (c)：坑 69 ~ 84（快照=提交点、B3 布局、g5 随机交错、g6 掉电四连：sync/换槽/sb_slot_/截断）
  data/batch4.js         第 4 批：坑 85 ~ 91
  data/batch5.js         第 5 批：坑 110~112 + r17-* / r18-*（hw2 / P2b / fsync / hole_fit / 护洞 / frag / coal / tidy）
```

```bash
python3 web/pits/build.py        # → ../../fcs-pits.html（≈400 KB+）
```

每个完整条目 = 左右同负载动画（含 **示例数据表** `tbl`）+ 代码对比 + 七节讲解 + **深析**（逐步推演 / 关键数字 / 走读）+ §12 原文。
短板故事板（<6 步）由 `template.html` 播放器自动加厚到 ≥6 步并补表示例数据。

侧栏分组含 R15–R18。打开根目录 `fcs-pits.html` 即可（无外部请求）。
