# Soft A1 frag 时空分布 · n=200k

探针：`make frag-trace` → `timeline.csv` + `holes.csv`  
分析：`scripts/analyze_frag_trace.py` / `analyze_frag_spatial.py` / `frag_chart_extract.py`

## Headline

| 量 | 值 |
|----|-----|
| commits | 603 |
| peak | **3354 @ c547**（upd，attr 含 HOLE_STARVE） |
| STARVE 事件 | **12**（ins 6 / upd 6） |
| choke（`n_ge64=0` ∧ `free≥64`） | **155** |
| `maxh<64` 提交 | 231 / 603 |
| `free_ge64/free_tot` 中位 | **0.412**（峰/starve 时 → **0**） |

峰不是“没洞”，是**洞全碎到 part 以下**且可配页被 PEND 锁住。

## 时间：STARVE 节律

前 8 次 starve 间隔 ≈ **64 commit**（`[64,64,63,65,65,65,64,…]`），每次 `d_tail≈311`。

典型 starve 快照（c65 / c129 / …）：

| | FREE | PEND |
|--|------|------|
| pages | 40–115 | **328**（偶 368） |
| segs | 2–11 | **8**（偶 16） |
| 片长 | maxh 29–40，**无 ≥64** | **8×41**（几乎完全齐整） |

解读：产出侧一次性挂出 **8 段 × 41 页** 的 PEND；同时 FREE 侧只剩 `<part` 残片 → `HOLE_STARVE`。峰前三拍（c544–547）PEND 从 96→0 释放，但释放进 FREE 后仍全是 **≤51** 片，ξ 不过 part。

## 空间：峰点 FREE 直方图（c547）

- **21 segs / 704 pages**，`max=51`，**ge64 = 0**
- 众数：**51×6、32×4、41×2、47×2** + 一串 1–9 的碎末
- 位置近似按 ~311 步长排开（381 / 692 / 1002 / 1354 / 1707 / 2018 / …）→ 与 commit/TRIM 节拍同族残渣，不是随机盐粒

## 假说对照（α 出生 / β PEND / γ 消费）

| 假说 | 证据 | 状态 |
|------|------|------|
| **β PEND 错峰** | starve 时 328 页锁在 8×41；FREE 几乎空 | **leading 主因（强）** |
| **α 出生形态** | 峰 FREE 众数 32/41/47/51，全 `<64`；步长对齐 d_tail | **leading 共因** |
| **γ 消费（P1d）** | 小洞 best-fit+skip 可削峰但不改出生/PEND 节律；A2 未过门 | 缓解刀，**非源头** |

`part_frag` / 直方图是反应量；根因在 **TRIM 前缀+页锚产生的残片尺度** 与 **PEND 与 part 请求的相位**。

## 探针注记

旧 `frag_trace` 在同提交既 peak 又 starve 时只打 `PEAK` 标签，STARVE 空间行被覆盖（本跑 c65/129/515 的 holes 全标 PEAK）。已改为峰/事件各打一份。重跑 `make frag-trace` 后 STARVE 标签才独立可见；**数值结论不依赖该标签**（timeline.attr 与 any-tag 空间一致）。

## Soft 默认

本分布解释 P1/P1b 抬峰、P1d 仅部分缓解：分配器改匹配，不改 41 页 PEND 出生与 ~64c 节律。**Soft 旋钮保持关**直至有改出生/相位的刀过 A1–A3。
