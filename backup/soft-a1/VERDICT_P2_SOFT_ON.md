# P2 Soft 默认开（坑 144）

拍板：`configure_deploy` / Soft 默认 `set_part_starve_fit(true)` + `set_starve_promote(true)`。
消融：argv / `set_*(false)` 仍可关。

## Soft A1（×3 中位 · off=显式关 · both=开）

| arm | med peak_x | med insert µs |
|-----|----------:|-------------:|
| off | 1.808 | 30.117 |
| both | 1.644 | 31.099 |
| Δ | -9.1% | +3.3% |

## 门禁（重跑）

- Soft sync insert：+7.3% 噪声内
- 升 Soft 默认判决：可议
- 全文：`VERDICT_P2_GATE.md`

## peers vs SQLite / LMDB

全文：`backup/peers-p2/PEERS_P2.md`（sync + async · 写/读/空间）。

## leading

- Soft **已开** P2 默认。
- Soft A1 峰 both/off -9.1%；insert +3.3%。
- 相对 SQL/LMDB：空间叙事不变；async 写仍碾压；sync range vs LMDB 缝仍在。

