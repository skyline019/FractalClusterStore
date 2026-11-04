# Soft range 根源诊断（路径 C · 坑 146）

> 不升 Soft 默认、不改读模型。数字：`backup/range-opt/root/` · `scripts/range_root.sh`。  
> 前置：坑 145 Cursor 微优化收益≈0（`VERDICT_RANGE.md`）。

## 1. 方程

```
range_amp ≈ k_way ≈ Σ_cells chains(intersecting families)
 Soft live 不付「仅为 range」的写 ⇒ 层合并只在 (a)dirty / (c)cheap / (b)cliff·t 家族 时付钱
 ⇒ 终态家族数常 < lvl_t 或顶层「一片家族很多页」⇒ 卡在 skip_need_t
 polish/cons = 强制 Σ→1
```

合并选组按 **家族**（同 lvl+seq），不是按片数（见 `level_pass` / DESIGN3 §5.3）。

## 2. 终态结构（本机一次 · Soft sync）

| N | arm | runs | fam 分布 (L0..L4) | cursors/scan | range µs | insert µs |
|---|-----|-----:|-------------------|-------------:|---------:|----------:|
| 50k | baseline | 10 | 6,2,2,0,0 | 12.38 | 81.3* | 10.2 |
| 50k | cliff1 | 10 | 同左 | 12.38 | 62.3* | 12.0 |
| 50k | cheap_inf | 10 | 同左 | 12.38 | 39.4* | 9.2 |
| 50k | lvl_t4 | 6 | 2,1,1,2,0 | 7.45 | 36.5* | 9.7 |
| 50k | polish | 1 | 1,0,0,0,0 | **1.25** | **4.6** | 6.0† |
| 200k | baseline | 30 | runs 0,3,2,**25**,0 · **fam 0,3,1,1,0** | 6.48 | 18.5 | 5.5 |
| 200k | cliff1 | 35 | fam 同形 | 6.31 | 16.2 | 7.9 |
| 200k | cheap_inf | 34 | fam 同形 | 6.31 | 16.8 | 7.4 |
| 200k | lvl_t4 | 7 | fam 分散 | **7.56↑** | 17.1 | 6.4 |
| 200k | polish | 1 | 1,… | **1.30** | **5.5** | 7.8† |

\* range 本机噪声大，看 **cursors/runs 结构** 比看绝对 µs。  
† polish 臂 insert 在 polish **之前**计量（与写路径同）；range 在 polish 后。

**关键**：200k baseline `runs=30` 但 `fam_hist` 顶层 **1 家族**（P2 同 seq 多片）。`(a)(b)(c)` 要 `fam.size()≥lvl_t`，单家族永远进不去。

## 3. merge_diag 归因（写+drain 累计）

| N | arm | skip_need_t | skip_not_trigger | did_cheap | did_cliff | did_reclaim | did_d |
|---|-----|------------:|-----------------:|----------:|----------:|------------:|------:|
| 50k | baseline | **369** | 0 | 20 | 0 | 0 | 0 |
| 200k | baseline | **1569** | 28 | 84 | 0 | 1 | 0 |
| 200k | cliff1 | 1614 | 0 | 84 | **1** | 0 | 0 |
| 200k | lvl_t4 | 1667 | **312** | 196 | 0 | 2 | 0 |

一句话：**Soft live 卡在 `skip_need_t`（层上家族数 < `lvl_t`）**；写中途 `did_cheap` 会开火，但终态 L0/各层又掉回 < t。  
`skip_not_trigger` 只在 200k 少量出现（满 t 但不 dirty/cheap 且未达 cliff）。  
`cliff1` / `cheap_inf` **几乎不改变终态 fam 结构**；`lvl_t4` 降 runs 但 200k **cursors 变差**（层铺开、同窗相交源更多）。

Soft A1 峰：**未跑**（实验臂无稳定 range 胜，不值得先烧峰）。

## 4. 路径 B：可选项（已落地）· 默认待门禁

API：`set_soft_consolidate(true)`（**默认 false**）。Soft + `maybe_tidy` 在 `tidy_soft_runs_` / layout 压力下走 `consolidate_step`（与 Hard 同刀、Soft 阈值）。  
`configure_deploy` / Soft 默认 **不**打开。另：`polish()` / `TidyPolicy::Hard|Periodic` 仍可用。

### 4.1 消融（`scripts/ablate_range_b.sh` · 一次）

| N | arm | runs | cursors | range µs | insert µs | peak_x (used/live) |
|---|-----|-----:|--------:|---------:|----------:|-------------------:|
| 50k | soft | 10 | 12.38 | 29.4 | **5.7** | **1.44** |
| 50k | soft_cons | 2 | 2.34 | 7.8 | 18.7（+3.3×） | 1.18 |
| 50k | hard | 2 | 2.34 | 10.6 | 20.1 | 1.18 |
| 50k | polish(末) | 1 | 1.25 | 3.9 | 7.7† | 3.07‡ |
| 200k | soft | 29 | 6.48 | 20.3 | **6.8** | **1.31** |
| 200k | soft_cons | 1 | 1.16 | 7.9 | **54.5（+8×）** | **5.22** |
| 200k | hard | 1 | 1.16 | 5.5 | 46.9 | 2.80 |
| 200k | polish(末) | 1 | 1.30 | 5.0 | 6.2† | 2.56‡ |

† insert 在 polish **前**计量。‡ polish 后 used 含归一产物，峰语义≠ Soft A1 写期峰。

**判读**：soft_cons **能**收 range/cursors，但 200k **写税与 peak_x 不合格** ⇒ **不得升 Soft 默认**。读服务更稳的是写完再 `polish`（写路径税低、range 上界）。

### 4.2 升默认门禁（全部过才议 Soft 默认开 soft_consolidate）

1. **消融**：`ablate_range_b` Soft vs Soft+cons — range/cursors 明显胜，且 insert 中位税 ≤~15%（与 P2 门禁同量级），200k peak_x 不劣于 Soft+噪声。  
2. **性能**：Soft peers sync（bench4/7）range 相对 Soft 基线改善；insert/update 税过门。  
3. **空间**：`make soft-a1-p2-gate`（或等价 Soft A1）峰不抬；A2 小盘不回归。  
4. **契约**：改 [CONSTRAINTS.md](../CONSTRAINTS.md) 显式允许 Soft 默认整理步进，并更新 EVAL17 写换读表述。

未过门 ⇒ 保持可选项；应用侧：`set_soft_consolidate(true)` + 循环 `maybe_tidy`，或对外前 `polish()`。

### 4.2b 动态整理（探讨 · 不升默认）

写中 duty/hyst 在 200k **抬高 cursors**；**写后 post** 写税≈Soft、range≈polish。全文 [`DISCUSS_RANGE_DYNAMIC.md`](DISCUSS_RANGE_DYNAMIC.md) · `ablate_range_dyn.sh`。  
leading：产品化优先 **写后/空闲补做**，勿把写路径 duty-cycle 当 Soft 默认。

### 4.3 路径 A

cliff/cheap/`lvl_t` 热拧仍 **不推荐**（§2–3）。远期「家族内过密」另号。

## 5. 非目标

- Cursor heap / k1 / 家族链微优化（坑 145 已结）
- 默改 cliff/脏度默认（CONSTRAINTS 禁止回潮）
- Eager `soft_consolidate` **不**进 Soft 默认（坑 147）；**Post 已升 Soft 默认**（坑 151 · `VERDICT_POST_SOFT_ON.md`）

## 6. 复现

```bash
bash scripts/range_root.sh          # 结构 + merge_diag
bash scripts/ablate_range_b.sh      # 路径 B 可选项消融
# → backup/range-opt/root/
```
