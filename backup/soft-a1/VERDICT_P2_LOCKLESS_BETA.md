# Soft A1 β 锁外 fsync（坑 141）

协议：配对 off→both · warmup 弃 · setarch -R · ×5

| | med | IQR |
|--|----:|----:|
| Δpeak | -12.66% | 5.38 |
| Δinsert | +2.56% | 7.19 |
| Δupdate | -3.44% | 22.80 |
| prom_ms | 6.73 | 7.22 |
| prom count | 2 | — |

## leading

- **代码根**：β 旧路径在 `maint_unit` 持 XLock 时 `write_sb`（drain+双 sync）⇒ put 全堵。
- **刀**：`starve_prom_flush_` 锁内 fold+capture，锁外 `sb_flush`，再锁内 promote。
- 本轮 med Δpeak -12.66% · Δins +2.56%（对照坑 140 confirm pend64 peak≈−14%）。
- Soft 默认仍关 P2。

