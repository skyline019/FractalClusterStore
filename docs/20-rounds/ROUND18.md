# 第 18 刀：养洞护航 + 整理状态机

> 日期 2026-10-20。承接 ROUND17 §2.5 / 讨论「常驻整理态」。  
> 代码：`set_protect_merge_holes` / `TidyPolicy` / `configure_deploy`+`polish`；测试 `g18`；探针 `layout` 增 `protect` / `budget_ms` / `units` 参数。  
> 日志：`backup/r18/layout_ab.log`、`layout_det.log`。

## 1. 做了什么

| 项 | 内容 |
|---|---|
| **护洞** | 用户 `alloc_pages`（flush / 溢出）跳过 ≥`hole_protect_pages`（默认 64）的**中部**大洞；`begin_stream` / 归并 finish / `maint=true` 仍可进洞；NOSPACE 自动退回吃洞。非 `M_RUN_HOLE`（格式 P2c 另轮）。 |
| **整理状态机** | `TidyPolicy::{Off,Soft,Hard,Periodic}` + `tidy_snap` / `maybe_tidy`；Soft=`configure_deploy`（async+cheap+hole_fit+护洞+hw）；Hard→`consolidate_step`；Periodic→`compact_file_step`；`polish`=重锤。 |
| **回归** | `g18`：护洞单元行为 + Soft/Hard/Periodic 烟测。`g14`/`g16` 复跑绿。 |

## 2. layout 峰值 A/B（护洞开/关）

命令：`build/layout N 500 32 1 1 0 pct protect [budget_ms units]`

### 2.1 默认 mode1（20 ms 预算，与 R17 主表同口径）

| 配置 | 峰值×（2 次） | skips | 设备写 |
|---|---|---|---|
| protect=0 pct0 | **1.96 / 1.96** | 0 | 41.5 MB |
| protect=1 pct0 | 1.97 / 1.97 | 23 | 41.5 MB |
| protect=0 pct150 | peak 2.03 / 1.94；末态 1.51 / 1.47 | 0 | 43.3~43.6 |
| protect=1 pct150 | peak 2.02 / 2.05；末态 1.53 / 1.51 | 56~60 | 43.9 |

**结论**：200k 默认路径上护洞**不降峰**（略噪或略差）；跳过计数在动，写量不变。

### 2.2 确定性（budget=0 + units=8，同 g17 `t_hole_fit`）

| 配置 | 峰值× | skips | 写 |
|---|---|---|---|
| protect=0 | **1.82**（×2 完全相同） | 0 | 41.8 |
| protect=1 | **1.81**（×2 相同） | 15 | 41.8 |

噪声内 −0.01×；机制生效，峰值主因仍不是「L0 吃掉唯一大洞」。

### 2.3 800k pct0（各 1 次）

| 配置 | 峰值× | skips | 写 |
|---|---|---|---|
| protect=0 | 1.53 | 0 | 171.0 |
| protect=1 | **1.49** | 839 | 171.0 |

弱收益约 **−0.04×**，零写放大。

## 3. 解读（对 ROUND17 §2.5 的修正）

§2.5 把 200k 残余 ~0.4× 归因于「插入期 L1 把 L1→L2 释放的洞吃掉」。护洞按字面堵住了用户路径进大洞（skips>0），但：

1. **200k 峰几乎不动** ⇒ 峰仍主要由「分片养洞（N 个小 TRIM 洞）+ 中等产物落尾」主导；hole_fit 已吃掉能动的那一刀。
2. 护洞会让 L0 **更常长尾**（grows 34→38 / 27→33），洞留给合并却未必在同一窗口被 `choose_part` 用上 ⇒ 默认预算下甚至略抬峰。
3. **800k** 洞更碎、跳过更多（839），才看到稳定但小的 −0.04×。

因此：护洞保留为 Soft 默认（**零写放大、机制正确、大库略益**），**不当作 200k 峰值 KPI**。真要再压 200k 峰值，下一刀仍是分配/稀疏 run（P2c）或养洞侧合并洞，不是再拧护洞门槛。

## 4. 产品默认

- **Soft 常驻**（`configure_deploy` / `set_tidy_policy(Soft)`）：读友好旋钮 + 护洞 + 可选 hw_reclaim。
- 对外读服务前 **`polish`**（或 Periodic + `maybe_tidy` 循环）。
- Hard：显式 opt-in（写放大↑）。

## 5. 养洞合并 A：碎片感知切片（已落地）

`choose_part`：当 `largest_hole < part_cap(part_lo)`、但 `free_page_count ≥ part_cap(part_lo)`、且最大洞 ≥ `part_cap(frag_floor)`（默认 16）⇒ 临时在 `[frag_floor, part_lo)` 按洞挑片；计数 `part_frags()`。默认开（`FCS_PART_FRAG=1`）；`set_part_frag(false)` 关。

### 5.1 layout A/B（`backup/r18/layout_frag.log`；det = budget0+units8）

| 配置 | 峰值× | runs | part_frags | 写 |
|---|---|---|---|---|
| frag=0 det200k | **1.82** | 23 | 0 | 41.8 |
| frag=1 det200k | **1.71** | 32 | 2 | 41.8 |
| frag=0 800k | 1.50 | 43 | 0 | 171.0 |
| frag=1 800k | 1.49 | 54 | 9 | 171.1 |

**结论**：200k **−0.11×**（真收益，零写放大）；代价 run +9；800k 噪声级。g16/g17/g18 绿。

## 6. 养洞合并 B：有界并洞（已落地，默认关）

预留前：`largest < part_cap(part_lo)` 且合计空闲够 ⇒ 每提交搬开一条挡路小 run（≤`merge_budget`，不嵌套作业；每作业 ≤4 次）。`part_coalesces()` 观测。顺序：B 在 A/`choose_part` 之前（否则 A 缩片后 B 永不触发）。

### 6.1 layout A/B（`backup/r18/layout_coal.log`；frag=1）

| 配置 | 峰值× | coalesces | 写 |
|---|---|---|---|
| coal=0 det200k | **1.71** | 0 | 41.8 |
| coal=1 det200k | **1.74** | 4 | 42.4 |
| coal=0 800k | **1.48** | 0 | 171.1 |
| coal=1 800k | **1.52** | 4 | 172.1 |

**否定**：B 在 layout 上抬峰 + 写放大（挡路 run 多写一遍，洞未必在同一窗口被片吃掉）。g18 `t_coal` 白盒证明机制正确。  
**产品**：`FCS_PART_COALESCE=0`；Soft **不**打开；显式 `set_part_coalesce(true)` 留给特殊负载。

## 7. 未做 / 已补

- `M_RUN_HOLE` 格式稀疏 run（P2_DESIGN：与合并峰弱相关）
- ~~Soft 下 peers 全量重跑~~ → **已做**（2026-10-21）：`backup/peers-soft/`（`make test` + Soft bench4/bench7）
- 护洞阈值扫描（已否定 200k KPI）
