# Soft A1 P2 税刀矩阵（坑 140 · 配对 Δ · 去 warmup）

门槛：med Δpeak ≤ −5% 且 med Δinsert ≤ +10%

| cfg | med Δpeak | IQR | med Δins | IQR | Δupd | fits | prom | gate |
|-----|----------:|----:|---------:|----:|-----:|-----:|-----:|:----:|
| F24 | -11.68% | 8.09 | +7.15% | 19.01 | -7.83% | 8 | 4 | PASS |
| F40 | -4.79% | 2.38 | +9.53% | 18.84 | +0.47% | 1 | 1 | fail |
| F40S8 | -6.61% | 4.71 | +5.39% | 27.91 | -2.21% | 1 | 1 | PASS |
| P2 | -12.97% | 6.17 | +11.74% | 39.84 | -1.36% | 4 | 4 | fail |
| S16 | -12.48% | 6.37 | +8.04% | 22.92 | +1.02% | 8 | 3 | PASS |
| S8 | -7.65% | 6.81 | +3.81% | 41.42 | -6.99% | 5 | 1 | PASS |
| cur | -5.70% | 11.01 | +6.28% | 29.52 | -1.89% | 4 | 2 | PASS |
| pend128 | -12.42% | 8.22 | +13.62% | 19.11 | +0.78% | 4 | 1 | fail |
| pend64 | -14.75% | 3.14 | +8.84% | 21.15 | -0.56% | 5 | 1 | PASS |

## ranking (score = −Δpeak − 0.5·max(Δins,0)；仅 PASS 可升默认)

- `pend64` score=10.33 peak=-14.75% ins=+8.84% [PASS] ← winner候选
- `S16` score=8.46 peak=-12.48% ins=+8.04% [PASS]
- `F24` score=8.10 peak=-11.68% ins=+7.15% [PASS]
- `P2` score=7.10 peak=-12.97% ins=+11.74% [fail]
- `S8` score=5.74 peak=-7.65% ins=+3.81% [PASS]
- `pend128` score=5.61 peak=-12.42% ins=+13.62% [fail]
- `F40S8` score=3.92 peak=-6.61% ins=+5.39% [PASS]
- `cur` score=2.56 peak=-5.70% ins=+6.28% [PASS]
- `F40` score=0.02 peak=-4.79% ins=+9.53% [fail]

## leading

- **Pareto 胜者**：`pend64`  peak med −14.75% · insert med +8.84%（IQR peak 最紧 3.1）
- 已写入 `FCS_PART_STARVE_PROM_MIN_PEND=64`；确认见 `VERDICT_P2_TAX_CONFIRM.md`（峰再测 ≈−14.2%）。
- Soft 默认仍关 P2。

