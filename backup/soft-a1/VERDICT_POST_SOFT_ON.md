# Soft 默认升 Post 门禁（坑 151）

判据：消融 insert≤+15% · cursors≤2 · Soft A1 peak≤+5% / insert≤+15% · peers sync range 优于 Soft 基线。

## 1. 消融（`ablate_range_idle` · 200k gap32）

| | Soft | Post | Post/Soft |
|--|------:|-----:|----------:|
| insert | 6.593 | 6.474 | 0.982× |
| cursors | 6.48 | 1.30 | — |
| 门禁 | | | PASS |

## 2. Soft A1（×3 中位 · 写期无 repay）

| arm | med peak_x | med insert µs |
|-----|----------:|-------------:|
| off | 1.647 | 30.905 |
| post | 1.654 | 34.728 |
| Δ | +0.4% | +12.4% |
| 门禁 | PASS | PASS |

## 3. peers Soft sync（`PEERS_POST.md`）

- range FCS/SQL：base **0.46×** → Post **0.12×** · PASS
- 全文：`backup/peers-post/PEERS_POST.md`

## 判决：**升 Soft 默认：可议通过 → 已落地（坑 151）**

`configure_deploy` / Soft → `SoftCons::Post`；读前 `soft_cons_repay` 或 `polish`；消融 `set_soft_cons(Off)`。

