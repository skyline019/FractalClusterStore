# P2 peers：FCS vs SQLite / LMDB（坑 144 Soft 默认开）

both = Soft/`configure_deploy` 默认 P2；off = argv 显式关。
比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。
复现：`bash scripts/peers_p2_ab.sh` · 源 `backup/peers-p2/`

## vs SQLite（bench4）

### Soft sync 50k

| 指标 | FCS off | FCS both | SQLite | off/SQL | both/SQL | both/off |
|------|--------:|---------:|-------:|--------:|---------:|---------:|
| insert µs/op | 6.057 | 6.714 | 6.344 | 0.92× | 1.10× | 1.11× |
| update µs/op | 7.157 | 7.875 | 9.662 | 0.87× | 0.71× | 1.10× |
| get µs/op | 0.480 | 0.558 | 1.752 | 0.29× | 0.30× | 1.16× |
| range µs/scan | 3.753 | 3.870 | 1.881 | 1.96× | 2.10× | 1.03× |
| space MB | 3.05 | 3.05 | 2.24 | 1.36× | 1.36× | 1.00× |

### Soft async+group+cons+shr 50k

| 指标 | FCS off | FCS both | SQLite | off/SQL | both/SQL | both/off |
|------|--------:|---------:|-------:|--------:|---------:|---------:|
| insert µs/op | 0.444 | 0.432 | 6.120 | 0.07× | 0.07× | 0.97× |
| update µs/op | 0.769 | 0.756 | 10.717 | 0.07× | 0.07× | 0.98× |
| get µs/op | 0.560 | 0.362 | 1.655 | 0.34× | 0.22× | 0.65× |
| range µs/scan | 0.725 | 0.715 | 1.857 | 0.36× | 0.42× | 0.99× |
| space MB | 2.14 | 2.14 | 2.24 | 0.96× | 0.96× | 1.00× |

## vs LMDB（bench7）

### Soft sync 50k

| 指标 | FCS off | FCS both | LMDB | off/LMDB | both/LMDB | both/off |
|------|--------:|---------:|-----:|---------:|----------:|---------:|
| insert µs/op | 6.543 | 5.474 | 6.262 | 1.04× | 0.87× | 0.84× |
| update µs/op | 8.624 | 5.580 | 13.503 | 0.64× | 0.41× | 0.65× |
| get µs/op | 0.615 | 0.561 | 0.292 | 2.11× | 1.92× | 0.91× |
| range µs/scan | 4.496 | 3.958 | 0.311 | 14.46× | 12.73× | 0.88× |
| space MB | 3.05 | 3.05 | 5.29 | 0.58× | 0.58× | 1.00× |

### Soft async+group+cons+shr 50k

| 指标 | FCS off | FCS both | LMDB | off/LMDB | both/LMDB | both/off |
|------|--------:|---------:|-----:|---------:|----------:|---------:|
| insert µs/op | 0.360 | 0.342 | 6.018 | 0.06× | 0.06× | 0.95× |
| update µs/op | 0.705 | 0.668 | 12.805 | 0.06× | 0.05× | 0.95× |
| get µs/op | 0.403 | 0.339 | 0.306 | 1.32× | 1.11× | 0.84× |
| range µs/scan | 0.721 | 0.687 | 0.334 | 2.16× | 2.06× | 0.95× |
| space MB | 2.14 | 2.14 | 5.29 | 0.41× | 0.41× | 1.00× |

## 读法

- **both/off**：P2 Soft 默认相对显式关的税/收益。
- **FCS/对手**：相对 SQLite/LMDB；&lt;1 写更快。
- Soft sync range vs LMDB 仍是「写换读」大缝；async+cons 整理后收窄。
- **坑 144：P2 已升 Soft 默认**（见 `VERDICT_P2_SOFT_ON.md`）。
