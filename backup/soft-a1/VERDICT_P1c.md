# Soft A1 · P1c `stream_best_fit` 消融判决

> 命令：`make soft-a1-bf`（N=200000 · inv=0）· 坑 134  
> 对照：P1/P1b 跳洞类已否（`VERDICT_P1.md` / `VERDICT_P1b.md`）

## 数字

### Soft A1 探针（`make soft-a1-bf`）

| 臂 | peak_tail | peak_x | end_x | HOLE_STARVE | stream_best_fits | hw_reclaims |
|----|-----------|--------|-------|-------------|------------------|-------------|
| **off** | 3415 | **1.854** | 1.469 | 12 | 0 | 19 |
| **on** | 3116 | **1.670** | 1.488 | 10 | 102 | 14 |

Δ peak_x ≈ **−9.9%**；peak_tail −299；end_x 略升（+0.02）。

### Layout A1（protect+hw150 · accel=0 · ±bf）

| 臂 | peak_tail | peak_x | end hw/live | fits |
|----|-----------|--------|-------------|------|
| **off** | 3353 | **1.80** | 1.46 | 0 |
| **on** | 3076 | **1.64** | 1.54 | 111 |

Δ peak_x ≈ **−8.9%**；末态略肥。

### A2 tiny_space（accel=0 · ±bf）

| pages | peak_x off | peak_x on | 判 |
|------:|-----------:|----------:|----|
| 256 | 1.052 | 1.052 | 平 |
| 512 | 1.040 | 1.050 | 微升 |
| 1024 | 1.483 | 1.488 | 微升 |

## 判决

**Soft A1 / Layout A1：峰正向。A2：中性偏微负。升 Soft 默认：否** —— 三画像未同向；A3 peers 未跑。

leading：大盘流式 best-fit 利养洞；小盘峰已贴 `user_lim`，best-fit 无空间可挖、略扰序。

## 延迟 / 读写（排除波动）

命令：`make soft-a1-bf-lat` / `ROUNDS=5 N=50000 scripts/bench_bf_latency_ab.sh`  
方法：Soft sync **交错** off/on×5；看 median；|Δ|≤max(半宽%, 3%) ⇒ 噪声内。  
明细：`latency_bf_ab.csv` · `latency_bf_ab.md`

| 指标 | off med | on med | Δ% | 判读 |
|------|--------:|-------:|---:|------|
| insert us/op | 4.537 | 5.074 | **+11.8%** | 变慢（贴半宽门） |
| update us/op | 7.102 | 5.470 | −23.0% | **噪声内**（off span 大） |
| get us/op | 0.484 | 0.496 | +2.5% | **噪声内** |
| range us/scan | 4.248 | 3.864 | −9.0% | **噪声内** |
| probes/get | 1.020 | 1.020 | 0 | 读放大不变 |

leading：best-fit 扫 `free_` 加在流式分配上 ⇒ 写路径可感；热 get/range 不走该路径 ⇒ 读侧噪声内。

## 产品面

- `configure_deploy` **仍不开** `stream_best_fit`（A2 未同向 + insert 中位偏慢）
- 旋钮保留；可议收窄（仅大盘）或接受写税换峰
- 机制回归：`c5` · 延迟：`make soft-a1-bf-lat`

## 与前刀对照

| 刀 | Soft A1 peak_x on/off | Soft 默认 |
|----|----------------------|-----------|
| P1 整洞跳过 | 4.15 / 1.80 | 禁 |
| P1b anti-shatter | 2.20 / 1.86 | 禁 |
| **P1c best-fit** | **1.67 / 1.85** | **待三画像** |
