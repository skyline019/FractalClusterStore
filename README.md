# FractalClusterStore

FractalClusterStore（简称 **FCS**）是面向嵌入式设备与单进程服务场景的**有序键值存储内核**。本仓库产品实现为 XDB v3，主接口位于 `include/fcs.h`。

[![Strict CI](https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml/badge.svg)](https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/skyline019/FractalClusterStore)](https://github.com/skyline019/FractalClusterStore/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

当前公开版本：[v0.1.0](https://github.com/skyline019/FractalClusterStore/releases/tag/v0.1.0)。

---

## 1. 定位与适用范围

| 项目 | 说明 |
|---|---|
| 数据模型 | 固定宽度键（`u64`）的有序 KV；支持点查、区间与前缀扫描、快照视图与二级索引 |
| 部署形态 | 库内嵌、单进程；由调用方提供块设备抽象（内存或文件） |
| 非目标 | 通用 SQL、多进程共享同一打开库、分布式复制 |

FCS 采用追加型 **run** 组织数据，目录层次为 cell → segment → run。写入以追加为主，读路径经布隆门控与段级摘要剪枝；空间与读放大由分层合并及整理策略（含 Soft 部署形态）约束。其设计目标不同于页内定位式 B+ 树，亦非替代完整关系型引擎。

适用场景包括：设备侧本地状态、同机服务的持久化有序表、以及对写放大与提交阻塞敏感、但不需要 SQL 语义的嵌入式路径。

明确边界：键类型固定为 `u64`；无跨进程锁协议；长时 soak 与真实拔电矩阵见评估文档中的缺口说明，不宜在未阅读的情况下直接用于严苛耐久合同。

---

## 2. 体系结构概要

```
应用
  └── Store（include/fcs.h）
        ├── 内存表 → 提交落盘为 L0 run
        ├── cell 目录 / 段 / run（布隆、围栏、键域摘要）
        ├── 清单与双槽超级块（崩溃恢复）
        └── 维护：分层合并 · 分片 TRIM · Soft/Hard/Periodic 整理
              └── BlockDev（RamDev / FileDev / 自定义）
```

耐久路径依赖双槽超级块与提交点同步；页复用与可见性以已同步超级块及 `durable_vers()` 为准。提交路径可将 fsync 移出排他锁，以降低读者被提交阶段长时间阻塞的概率。部署入口可选用 `TidyPolicy::Soft` / `configure_deploy()`，在异步写、护洞与按洞分片等默认组合上给出常驻整理形态。

设计规格与缺陷登记见 [`docs/10-design/DESIGN3.md`](docs/10-design/DESIGN3.md)；轮次落地与取舍见 [`docs/00-entry/EVAL17.md`](docs/00-entry/EVAL17.md)。

---

## 3. 性能对照（摘要）

下列数字来自与 SQLite 3.46 同批、同负载的 bench4 对照（50 000 键、32 B 值、每 500 次操作提交；FCS 为异步写加整理形态；SQLite 为 `WAL + synchronous=FULL`）。完整条件、方差说明与复现命令见 [详情手册](docs/00-entry/DETAIL.md) 与 [EVAL17](docs/00-entry/EVAL17.md)。

| 指标 | FCS（异步 + Soft 整理） | SQLite 3.46 |
|---|---:|---:|
| 写入（含提交） | 0.51 µs/op | 2.1–2.2 |
| 覆盖写（含提交） | 0.83 | 10.8–18.2 |
| 点查 | 0.55 | 1.5–1.9 |
| 区间扫描（100 行） | 0.57 | 1.5–1.9 |
| 每记录占用 | 41.9 B | 46.9 B |

同仓库历史实现在可比写负载下的写放大大致为：v1 ≈ 56×，v2 ≈ 22×，v3（FCS）≈ 2.7×。相对 LMDB 等只读优化引擎，产品文档将读路径定位为「写换读」权衡，整理前后区间扫描差异显著，选型时应以 EVAL17 全表为准。

---

## 4. 功能范围

**数据操作**  
`put` / `del` / `del_range` / `get`；可重定位游标；`select`、前缀扫描与按版本的快照视图；二级索引与主记录同一次提交提交，语义上原子。

**事务与持久化**  
逻辑组提交、可选异步刷盘；`durable_vers()` 反映已落盘版本；崩溃后按超级块与清单恢复。

**空间与维护**  
分层合并、分片 TRIM、护洞及 Soft / Hard / Periodic 整理策略；对外提供读服务前可调用 `polish()` 收口。维护模式支持提交后预算、调用方驱动与后台线程等形式。

**校验与观测**  
`verify` / `verify_ownership` / `scrub`；追踪环与维护统计接口。回归覆盖掉电注入、挂载模糊、多写者与黄金镜像等（见测试套件）。

**容量**  
页号 32 位、页大小 4 KB 时，单库寻址量级约 16 TB；单值长度上限 65 534 字节。

---

## 5. 构建与最小示例

依赖：C++17、`g++`、`make`（Linux / WSL 为主要验证环境）。

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

公共头文件：`include/fcs.h`，以及 `blockdev.h`、`layout.h`、`common.h`。持久化文件场景使用 `FileDev`。完整 API、旋钮默认值与失败语义见 [详情手册](docs/00-entry/DETAIL.md)。

严格 CI（GitHub Actions）要求 Compile（`-Werror`）、Smoke、全量 `make test`、Sanitizers 四门均通过，聚合检查名为 **Strict CI OK**。

---

## 6. 文档与导航

| 文档 | 内容 |
|---|---|
| [详情手册](docs/00-entry/DETAIL.md) | API 摘录、基准表、轮次状态与测试矩阵 |
| [EVAL17](docs/00-entry/EVAL17.md) | 正确性、性能、空间与风险的冻结评估 |
| [DESIGN3](docs/10-design/DESIGN3.md) | 现行设计主文与缺陷表 |
| [文档索引](docs/README.md) | 轮次记录、讨论稿、LaTeX 技术文档 |
| [v0.1.0 发布说明](docs/00-entry/RELEASE_NOTES_v0.1.0.md) | 首个公开里程碑说明 |
| [fcs-anim.html](fcs-anim.html) / [fcs-pits.html](fcs-pits.html) | 结构与缺陷对照的离线讲解页 |

---

## 7. 目录结构

```
include/     引擎与块设备、页布局头文件
tests/       回归套件（make test）
bench/       基准与对照程序
docs/        评估、设计、轮次与讨论
web/         讲解页源码
scripts/     本地 CI 复现辅助脚本
```

仓库中仍保留 v1（`../src/engine.cpp`）与 v2（`include/store.h`）实现，供对照与历史基准；**产品默认路径为 v3 / FCS**。

---

## 8. 许可与协作

本项目以 [MIT License](LICENSE) 发布。

- 问题跟踪：https://github.com/skyline019/FractalClusterStore/issues  
- 发行版：https://github.com/skyline019/FractalClusterStore/releases  
