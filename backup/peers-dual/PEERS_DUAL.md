# Soft Post × dual_mem vs SQLite / LMDB（坑 156）

产品 Soft = Post + `soft_cons_repay`；`dual_mem` 可选（仅 sync 走 `flush_mem_dual`）。
比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。
复现：`bash scripts/peers_dual_ab.sh` · 源 `backup/peers-dual/`

## vs SQLite · Soft sync Post 50k

| 指标 | dual=off | dual=on | SQLite | off/SQL | on/SQL | on/off |
|------|--------:|--------:|-------:|--------:|-------:|-------:|
| insert µs/op | 7.453 | 6.136 | 6.888 | 1.08× | 0.89× | 0.82× |
| update µs/op | 8.485 | 5.910 | 16.400 | 0.52× | 0.36× | 0.70× |
| get µs/op | 0.455 | 0.352 | 2.238 | 0.20× | 0.16× | 0.77× |
| range µs/scan | 1.188 | 0.554 | 1.805 | 0.66× | 0.31× | 0.47× |
| space MB | 2.50 | 2.50 | 2.24 | 1.12× | 1.12× | 1.00× |
| dual flushes / excl_us / settle_us | — | 100 / 8639 / 1036 | — | — | — | — |

## vs SQLite · Soft async+group+cons+shr Post 50k

| 指标 | dual=off | dual=on | SQLite | off/SQL | on/SQL | on/off |
|------|--------:|--------:|-------:|--------:|-------:|-------:|
| insert µs/op | 0.432 | 0.540 | 5.615 | 0.08× | 0.10× | 1.25× |
| update µs/op | 0.791 | 0.578 | 13.205 | 0.06× | 0.04× | 0.73× |
| get µs/op | 0.382 | 0.364 | 1.785 | 0.21× | 0.20× | 0.95× |
| range µs/scan | 0.561 | 0.559 | 1.917 | 0.29× | 0.29× | 1.00× |
| space MB | 2.15 | 2.15 | 2.24 | 0.96× | 0.96× | 1.00× |
| dual flushes / excl_us / settle_us | — | 1 / 7299 / 536 | — | — | — | — |

## vs LMDB · Soft sync Post 50k

| 指标 | dual=off | dual=on | LMDB | off/LMDB | on/LMDB | on/off |
|------|--------:|--------:|-----:|---------:|--------:|-------:|
| insert µs/op | 5.666 | 6.225 | 6.678 | 0.85× | 0.93× | 1.10× |
| update µs/op | 6.668 | 5.641 | 11.194 | 0.60× | 0.50× | 0.85× |
| get µs/op | 0.471 | 0.307 | 0.306 | 1.54× | 1.00× | 0.65× |
| range µs/scan | 0.694 | 0.553 | 0.338 | 2.05× | 1.64× | 0.80× |
| space MB | 2.50 | 2.50 | 5.29 | 0.47× | 0.47× | 1.00× |
| dual flushes / excl_us / settle_us | — | 100 / 8895 / 1104 | — | — | — | — |

## vs LMDB · Soft async+group+cons+shr Post 50k

| 指标 | dual=off | dual=on | LMDB | off/LMDB | on/LMDB | on/off |
|------|--------:|--------:|-----:|---------:|--------:|-------:|
| insert µs/op | 0.348 | 0.466 | 5.636 | 0.06× | 0.08× | 1.34× |
| update µs/op | 0.595 | 0.565 | 9.228 | 0.06× | 0.06× | 0.95× |
| get µs/op | 0.305 | 0.391 | 0.326 | 0.94× | 1.20× | 1.28× |
| range µs/scan | 0.508 | 0.743 | 0.337 | 1.51× | 2.20× | 1.46× |
| space MB | 2.15 | 2.15 | 5.29 | 0.41× | 0.41× | 1.00× |
| dual flushes / excl_us / settle_us | — | 1 / 7301 / 469 | — | — | — | — |

## 读法

- **on/off**：dual 相对产品 Soft Post；≈1 = 中性；sync 才应见 dual flush 计数。
- **async+dual**：引擎不走 dual 路径（`try_dual` 要求 `!async_`）⇒ on≈off 属预期。
- Soft 默认仍 **关** dual（Negate-keep）。
