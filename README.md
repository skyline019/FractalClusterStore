# FractalClusterStore

**嵌入式有序 KV 引擎** —— 为设备侧与单进程服务设计的追加型存储内核。  
产品名 FCS；实现即本仓库的 **XDB v3**（`include/fcs.h`）。

[![Strict CI](https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml/badge.svg)](https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/skyline019/FractalClusterStore)](https://github.com/skyline019/FractalClusterStore/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## 它解决什么

在嵌入式或同机服务里，你往往需要：

- **有序键值**（点查、区间扫、前缀、快照时间旅行）
- **提交可持久、崩溃可恢复**，又不想背一整套 SQL / WAL 的重量
- **写得多、覆盖多**时，仍压得住写放大与提交对读者的阻塞

FCS 用 **cell → segment → run** 的分形布局：写入追加成 run，读路径布隆门控，分层合并 + Soft 整理态管空间。不是 B+ 树，也不是通用 SQL。

---

## 为什么选它

| | FCS（异步 + Soft 整理） | SQLite 3.46（同批对照） |
|---|---|---|
| 写入（含提交） | **0.51 µs/op** | 2.1~2.2 |
| 覆盖写 | **0.83** | 10.8~18.2 |
| 点查 | **0.55** | 1.5~1.9 |
| 区间扫（100 行） | **0.57** | 1.5~1.9 |
| 每记录字节 | **41.9 B** | 46.9 B |

50k 键 / 32 B 值 / 每 500 次提交；完整矩阵与复现命令见 [详情手册](docs/00-entry/DETAIL.md) 与 [EVAL17](docs/00-entry/EVAL17.md)。

相对 v1/v2 同负载写放大：**56× → 22× → ~2.7×**。

---

## 60 秒上手

```cpp
#include "fcs.h"
using namespace fcs;

RamDev dev(4096);
Store s;
u8 arena[512 * PAGE];
s.open(&dev, arena, 512);

s.set_tidy_policy(Store::TidyPolicy::Soft);  // 部署向默认：async + 护洞 + hole_fit …
s.put(42, "hello", 5);
s.commit();

u8 buf[64]; u16 n = 0;
s.get(42, buf, sizeof buf, &n);              // 点查
```

```sh
make          # 全量回归
make ci-smoke # 与 CI 对齐的快速门
make asan     # ASan / UBSan / TSan
```

头文件：`include/fcs.h`（及 `blockdev.h` / `layout.h` / `common.h`）。  
需要文件设备时用 `FileDev`；完整 API、旋钮与容量边界见 [详情手册](docs/00-entry/DETAIL.md)。

---

## 能力一览

- **读写**：`put` / `del` / `del_range` / `get`；游标与 `select` / 前缀 / 快照视图；二级索引同提交原子
- **提交**：组提交、异步刷盘、`durable_vers()`；fsync 出锁，降低读者被提交挡住的时间
- **空间**：分层合并、分片 TRIM、护洞与 Soft / Hard / Periodic 整理态；`polish()` 对外读前收口
- **可靠**：双槽超级块、掉电与模糊回归（`g6`/`h1`/`h3`…）、`verify` / `scrub`
- **规模**：单库至 **16 TB** 级页寻址（32 位页号 × 4 KB）；值最长 65534 B

边界（请先读再上线）：**单进程**、键固定 **u64**；无多进程锁、无小时 soak / 真拔电矩阵。取舍见 EVAL17。

---

## 文档地图

| 文档 | 用途 |
|---|---|
| **[详情手册](docs/00-entry/DETAIL.md)** | API、基准表、轮次状态、测试矩阵（原根 README） |
| [EVAL17](docs/00-entry/EVAL17.md) | 全量评估结论（正确性 / 读写 / 空间） |
| [DESIGN3](docs/10-design/DESIGN3.md) | 设计主文与坑表 |
| [docs 索引](docs/README.md) | ROUND / 讨论稿 / LaTeX |
| [v0.1.0 发布说明](docs/00-entry/RELEASE_NOTES_v0.1.0.md) | 首个公开里程碑 |
| [动画讲解](fcs-anim.html) · [坑库](fcs-pits.html) | 浏览器打开即可（无服务器） |

---

## 仓库结构（简）

```
include/     fcs.h 与设备/页布局
tests/       回归（make test）
bench/       基准与对照
docs/        评估 · 设计 · 轮次
web/         动画页源码
```

历史实现：v1（`../src/engine.cpp`）、v2（`include/store.h`）仍在树中，**产品默认是 v3 / FCS**。

---

## 许可

[MIT](LICENSE) · [Issues](https://github.com/skyline019/FractalClusterStore/issues) · [Releases](https://github.com/skyline019/FractalClusterStore/releases)
