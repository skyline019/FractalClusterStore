# Soft A1 P2 税刀确认（坑 140 · pend64 升默认后 ×5 配对）

协议：每轮 off→cur→pend64；warmup 弃；`setarch -R`；配对 Δ%。

| cfg | med Δpeak | IQR | med Δins | IQR | Δupd | fits | prom |
|-----|----------:|----:|---------:|----:|-----:|-----:|-----:|
| cur | -9.89% | 8.78 | +6.74% | 32.06 | -2.28% | 4 | 2 |
| pend64 | **-14.24%** | 12.78 | -26.88%* | 77.88 | -14.71% | 5 | 1 |

\*insert 配对 Δ IQR 极大（off 基线 27–40 µs 抖动）⇒ **勿报 insert 加速**；只认「不差于矩阵 +9% 量级」。

## vs 矩阵

| | matrix pend64 | confirm pend64 |
|--|-------------:|---------------:|
| med Δpeak | −14.75% | −14.24% |
| IQR peak | 3.14 | 12.78 |
| med Δins | +8.84% | （噪声） |

峰向 **两次独立批次同向 ≈−14~−15%**，相对 cur（≈−6~−10%）更深。

## 默认（实验臂；Soft 仍关）

`STARVE_FLOOR=32` · `MAX_SLICES=12` · `MAX_PROMOTES=1` · **`PROM_MIN_PEND=64`**

## leading

- 深入刀：β 加 `pending_free≥64` 门槛，少打空 `commit_delta`，峰更深且矩阵 IQR 更紧。
- Soft 默认仍关（坑 138 sync update 门禁）。
