# FractalClusterStore

<p align="center">
  <strong>嵌入式有序键值存储内核</strong><br>
  <sub>产品名 FCS · 实现 XDB v3 · <code>include/fcs.h</code></sub>
</p>

<p align="center">
  <a href="https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml"><img src="https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml/badge.svg" alt="Strict CI"></a>
  <a href="https://github.com/skyline019/FractalClusterStore/releases/tag/v0.1.0"><img src="https://img.shields.io/github/v/release/skyline019/FractalClusterStore" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
</p>

FractalClusterStore（**FCS**）面向嵌入式设备与单进程服务，提供可持久化的有序 KV 能力。本仓库的产品路径为 XDB v3。

| | |
|:---|:---|
| **公开版本** | [v0.1.0](https://github.com/skyline019/FractalClusterStore/releases/tag/v0.1.0) |
| **质量门禁** | Strict CI：Compile（`-Werror`）→ Smoke → Full test → Sanitizers → **Strict CI OK** |
| **许可** | [MIT](LICENSE) |

---

## 目录

1. [定位与适用范围](#1-定位与适用范围)
2. [体系结构概要](#2-体系结构概要)
3. [性能对照](#3-性能对照摘要)
4. [功能范围](#4-功能范围)
5. [构建与最小示例](#5-构建与最小示例)
6. [文档与导航](#6-文档与导航)
7. [目录结构](#7-目录结构)
8. [许可与协作](#8-许可与协作)

---

## 1. 定位与适用范围

| | 说明 |
|:---|:---|
| **数据模型** | 固定宽度键（`u64`）有序 KV；点查、区间 / 前缀扫描、快照视图、二级索引 |
| **部署形态** | 库内嵌、单进程；调用方提供块设备抽象（内存或文件） |
| **非目标** | 通用 SQL；多进程共享同一打开库；分布式复制 |

数据以追加型 **run** 组织，目录层次为 **cell → segment → run**。写入以追加为主；读路径经布隆门控与段级摘要剪枝；空间与读放大由分层合并及整理策略（含 Soft 部署形态）约束。设计目标不同于页内定位式 B+ 树，亦非完整关系型引擎的替代品。

**适用场景：** 设备侧本地状态、同机服务的持久化有序表，以及对写放大与提交阻塞敏感、但不需要 SQL 语义的嵌入式路径。

> **边界：** 键类型固定为 `u64`；无跨进程锁协议。长时 soak 与真实拔电矩阵的缺口见评估文档；未阅读相关说明前，不宜直接用于严苛耐久合同。

---

## 2. 体系结构概要

```text
应用
 └── Store                    include/fcs.h
      ├── 内存表               提交落盘为 L0 run
      ├── cell / 段 / run      布隆 · 围栏 · 键域摘要
      ├── 清单 + 双槽超级块    崩溃恢复
      ├── 维护                 分层合并 · 分片 TRIM · Soft / Hard / Periodic
      └── BlockDev             RamDev · FileDev · 自定义
```

| 主题 | 要点 |
|:---|:---|
| **耐久** | 双槽超级块与提交点同步；页复用与可见性以已同步超级块及 `durable_vers()` 为准 |
| **提交** | 可将 fsync 移出排他锁，降低读者被提交阶段长时间阻塞的概率 |
| **部署** | `TidyPolicy::Soft` / `configure_deploy()`：异步写、护洞、按洞分片等默认组合 |

规格与缺陷登记：[`DESIGN3.md`](docs/10-design/DESIGN3.md)  
轮次落地与取舍：[`EVAL17.md`](docs/00-entry/EVAL17.md)

---

## 3. 性能对照（摘要）

与 SQLite 3.46 **同批、同负载** 的 bench4 对照：50 000 键、32 B 值、每 500 次操作提交；FCS 为异步写加整理形态；SQLite 为 `WAL + synchronous=FULL`。完整条件、方差与复现命令见 [DETAIL](docs/00-entry/DETAIL.md)、[EVAL17](docs/00-entry/EVAL17.md)。

| 指标 | FCS（异步 + Soft） | SQLite 3.46 |
|:---|---:|---:|
| 写入（含提交） | 0.51 µs/op | 2.1–2.2 |
| 覆盖写（含提交） | 0.83 | 10.8–18.2 |
| 点查 | 0.55 | 1.5–1.9 |
| 区间扫描（100 行） | 0.57 | 1.5–1.9 |
| 每记录占用 | 41.9 B | 46.9 B |

| 世代 | 可比写负载下的写放大（约） |
|:---|---:|
| v1 | 56× |
| v2 | 22× |
| **v3（FCS）** | **2.7×** |

相对 LMDB 等偏只读优化的引擎，文档将读路径定位为「写换读」权衡；整理前后区间扫描差异显著，选型以 EVAL17 全表为准。

---

## 4. 功能范围

| 类别 | 内容 |
|:---|:---|
| **数据操作** | `put` / `del` / `del_range` / `get`；可重定位游标；`select`、前缀扫描、按版本快照视图；二级索引与主记录同一次提交，语义原子 |
| **事务与持久化** | 逻辑组提交、可选异步刷盘；`durable_vers()`；崩溃后按超级块与清单恢复 |
| **空间与维护** | 分层合并、分片 TRIM、护洞；Soft / Hard / Periodic；`polish()` 对外读前收口；提交后预算 / 调用方驱动 / 后台线程 |
| **校验与观测** | `verify` / `verify_ownership` / `scrub`；追踪环与维护统计；掉电、挂载模糊、多写者、黄金镜像等回归 |
| **容量** | 页号 32 位 × 页 4 KB → 单库寻址约 16 TB；单值上限 65 534 字节 |

---

## 5. 构建与最小示例

**依赖：** C++17、`g++`、`make`（主要验证环境：Linux / WSL）

```sh
make            # 全量回归
make ci-smoke   # 与 CI 烟雾门对齐
make ci         # 本地对齐严格 CI（不含 sanitizers）
make asan       # ASan / UBSan / TSan
```

```cpp
#include "fcs.h"
using namespace fcs;

RamDev dev(4096);
Store s;
u8 arena[512 * PAGE];
if (s.open(&dev, arena, 512) != RC::OK) return 1;

s.set_tidy_policy(Store::TidyPolicy::Soft);
s.put(42, "hello", 5);
if (s.commit() != RC::OK) return 1;

u8 buf[64];
u16 n = 0;
s.get(42, buf, sizeof buf, &n);
```

| 项 | 路径 / 说明 |
|:---|:---|
| 主头文件 | `include/fcs.h` |
| 附属头文件 | `blockdev.h` · `layout.h` · `common.h` |
| 文件设备 | `FileDev` |
| 完整 API | [详情手册](docs/00-entry/DETAIL.md) |

---

## 6. 文档与导航

| 文档 | 内容 |
|:---|:---|
| [详情手册](docs/00-entry/DETAIL.md) | API 摘录、基准表、轮次状态、测试矩阵 |
| [EVAL17](docs/00-entry/EVAL17.md) | 正确性 / 性能 / 空间 / 风险（冻结评估） |
| [DESIGN3](docs/10-design/DESIGN3.md) | 现行设计主文与缺陷表 |
| [文档索引](docs/README.md) | 轮次记录、讨论稿、LaTeX |
| [v0.1.0 说明](docs/00-entry/RELEASE_NOTES_v0.1.0.md) | 首个公开里程碑 |
| [fcs-anim.html](fcs-anim.html) · [fcs-pits.html](fcs-pits.html) | 离线结构与缺陷对照讲解 |

---

## 7. 目录结构

```text
include/      引擎、块设备与页布局头文件
tests/        回归套件（make test）
bench/        基准与对照
docs/         评估 · 设计 · 轮次 · 讨论
web/          讲解页源码
scripts/      本地 CI 复现辅助
```

历史实现 v1（`../src/engine.cpp`）与 v2（`include/store.h`）仍保留于树中，供对照与历史基准。**产品默认路径为 v3 / FCS。**

---

## 8. 许可与协作

以 [MIT License](LICENSE) 发布。

| | |
|:---|:---|
| Issues | https://github.com/skyline019/FractalClusterStore/issues |
| Releases | https://github.com/skyline019/FractalClusterStore/releases |
