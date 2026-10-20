# docs — Markdown 分类索引

产品文档按**阅读角色**分目录；旧文件名（`docs/EVAL17.md`、`docs/ROUND18.md` 等）保留一页跳转 stub，兼容历史链接。

```
docs/
├── README.md              ← 本索引
├── 00-entry/              ← 从这里读（评估 / 升格）
├── 10-design/             ← 设计真相与规格
├── 20-rounds/             ← 各轮执行记录（时间序）
├── 30-discuss/            ← 讨论稿（已落地或待拍板）
├── latex/                 ← XeLaTeX 详解（PDF）
└── *.md                   ← 旧路径 stub → 上列目录
```

---

## 0. 先读（入口）

| 路径 | 用途 |
|---|---|
| [`00-entry/EVAL17.md`](00-entry/EVAL17.md) | **冻结全量评估**（滚动结论；含 Soft peers） |
| [`00-entry/BATTLE.md`](00-entry/BATTLE.md) | 包 A/B/L 对照与升格说明 |

仓库根 [`../README.md`](../README.md) = 产品用法 + 部署旋钮；数字细节以 EVAL17 / DESIGN3 为准。

---

## 1. 设计（规格与坑清单）

| 路径 | 用途 |
|---|---|
| [`10-design/DESIGN3.md`](10-design/DESIGN3.md) | **现行设计主文**（基准矩阵、§12 坑表、轮次摘要） |
| [`10-design/DESIGN2.md`](10-design/DESIGN2.md) | v2 设计（历史） |
| [`10-design/P2_DESIGN.md`](10-design/P2_DESIGN.md) | P2 格式 v2 设计稿（P2a/b/c） |

---

## 2. 轮次记录（执行日志）

| 路径 | 主题 |
|---|---|
| [`20-rounds/ROUND13.md`](20-rounds/ROUND13.md) | 流式产物 / 层规则 / 预算·线程 |
| [`20-rounds/ROUND14.md`](20-rounds/ROUND14.md) | 作业增量账 / 读者让路 / 高水位 |
| [`20-rounds/ROUND15.md`](20-rounds/ROUND15.md) | 二步搬家；引出 fsync 讨论 + P2 设计 |
| [`20-rounds/ROUND16.md`](20-rounds/ROUND16.md) | P2a+P2b（FCS2 / `M_RUN_TRIM`） |
| [`20-rounds/ROUND17.md`](20-rounds/ROUND17.md) | fsync 出锁 / hole_fit / 家族游标 |
| [`20-rounds/ROUND18.md`](20-rounds/ROUND18.md) | 护洞 / Soft 整理态 / 碎片 A / 并洞 B |

阅读顺序：入口 EVAL17 → 需要细节时下钻对应 ROUND。

---

## 3. 讨论稿

| 路径 | 用途 |
|---|---|
| [`30-discuss/DISCUSS15_FSYNC.md`](30-discuss/DISCUSS15_FSYNC.md) | fsync 出锁讨论（R17 已落地） |

更早 DISCUSS / EVAL12 等已扫入归档（见下 §5）。

---

## 4. LaTeX 技术文档

| 路径 | 用途 |
|---|---|
| [`latex/README.md`](latex/README.md) | 编译说明 |
| `latex/chapters/` | 概述 · **代码详解** · 架构 · 踩坑… |
| `latex/build/main.pdf` | 编译产物（`cd latex && make`） |

面向「基础数据库知识」读者：先读 PDF 第「代码详解」章，再对 `include/fcs.h`。

---

## 5. 仓库内其它 Markdown（非 docs/ 正文）

### 5.1 对标与回归日志

| 路径 | 分类 |
|---|---|
| [`../backup/peers-soft/PEERS.md`](../backup/peers-soft/PEERS.md) | Soft 产品形态 peers（2026-10-21） |
| [`../backup/peers-r17/PEERS.md`](../backup/peers-r17/PEERS.md) | R17 基线 peers |
| [`../backup/wsl-20261020/PEERS.md`](../backup/wsl-20261020/PEERS.md) | 升格前 WSL 归档 |
| [`../backup/wsl-20261020/ARCHIVE.md`](../backup/wsl-20261020/ARCHIVE.md) | 该目录说明 |

### 5.2 历史文档归档（勿与产品主文混读）

总览：[`../backup/README.md`](../backup/README.md)。

| 路径 | 内容 |
|---|---|
| [`../backup/r18/swept/docs-hist/`](../backup/r18/swept/docs-hist/) | DISCUSS10–12、EVAL12、ROUND16_L_alt、EVAL14_L_alt… |
| [`../backup/r18/swept/CLEAN_MANIFEST.md`](../backup/r18/swept/CLEAN_MANIFEST.md) | 清扫搬家清单 |
| [`../backup/pre-battle-promote/`](../backup/pre-battle-promote/) | L 分叉快照 + 旧 ROUND16/EVAL14 |
| [`../backup/r14/ROUND14.md`](../backup/r14/ROUND14.md) | 轮次旁路副本 |
| [`../backup/r18/pre-comment-modular/RESTORE.md`](../backup/r18/pre-comment-modular/RESTORE.md) | 注释模块化还原 |

### 5.3 工程旁路 README

| 路径 | 用途 |
|---|---|
| [`../web/README.md`](../web/README.md) | pits HTML / 动画页 |
| [`../bench/probe/README.md`](../bench/probe/README.md) | layout 等探针 |
| [`../bench/probe/ARCHIVE.txt`](../bench/probe/ARCHIVE.txt) | 探针归档说明（含 storm.cpp） |

---

## 6. 分类原则（以后往哪放）

| 新文档类型 | 目录 |
|---|---|
| 滚动评估 / 升格结论 | `00-entry/` |
| 持久规格、坑表、格式设计 | `10-design/` |
| 「这一刀做了什么 + 数字」 | `20-rounds/ROUNDxx.md` |
| 未拍板或长讨论 | `30-discuss/` |
| 可排版详解 / 教学 | `latex/` |
| 一次性实测日志、对照表 | `backup/<topic>/`（配短 `PEERS.md` / `ARCHIVE.md`） |
| 已废止正文 | `backup/r18/swept/docs-hist/`（不要留在 `docs/` 根） |

---

## 7. 兼容 stub

下列文件名仍在 `docs/` 根下，内容仅为跳转：

`EVAL17.md` `BATTLE.md` `DESIGN2.md` `DESIGN3.md` `P2_DESIGN.md`  
`ROUND13.md` … `ROUND18.md` `DISCUSS15_FSYNC.md`
