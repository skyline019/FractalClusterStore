# Soft peers vs SQLite / LMDB（坑 151 · Soft 默认 Post）

**产品 Soft** = Post 列（写后 `soft_cons_repay`）。base = 消融 `soft_cons=Off`（写换读 live）。
比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。50k · B=500 · val=32B。
复现：`bash scripts/peers_post_ab.sh` · 源 `backup/peers-post/`

## vs SQLite（bench4）

### Soft sync 50k

| 指标 | FCS base | FCS Post | SQLite | base/SQL | Post/SQL | Post/base |
|------|--------:|---------:|-------:|---------:|---------:|----------:|
| insert µs/op | 6.771 | 6.416 | 5.479 | 1.23× | 1.18× | 0.95× |
| update µs/op | 6.378 | 5.791 | 10.979 | 0.69× | 0.46× | 0.91× |
| get µs/op | 0.674 | 0.374 | 1.747 | 0.39× | 0.21× | 0.55× |
| range µs/scan | 4.837 | 0.788 | 1.845 | 2.52× | 0.44× | 0.16× |
| space MB | 3.05 | 2.50 | 2.24 | 1.36× | 1.12× | 0.82× |

### Soft async+group+cons+shr 50k

| 指标 | FCS base | FCS Post | SQLite | base/SQL | Post/SQL | Post/base |
|------|--------:|---------:|-------:|---------:|---------:|----------:|
| insert µs/op | 0.917 | 0.316 | 5.167 | 0.18× | 0.06× | 0.34× |
| update µs/op | 1.267 | 0.658 | 10.143 | 0.14× | 0.06× | 0.52× |
| get µs/op | 0.409 | 0.403 | 1.686 | 0.24× | 0.24× | 0.99× |
| range µs/scan | 0.948 | 1.002 | 1.728 | 0.56× | 0.57× | 1.06× |
| space MB | 2.14 | 2.15 | 2.24 | 0.96× | 0.96× | 1.00× |

## vs LMDB（bench7）

### Soft sync 50k

| 指标 | FCS base | FCS Post | LMDB | base/LMDB | Post/LMDB | Post/base |
|------|--------:|---------:|-----:|----------:|----------:|----------:|
| insert µs/op | 6.009 | 5.436 | 6.237 | 0.96× | 0.87× | 0.90× |
| update µs/op | 6.640 | 6.066 | 12.359 | 0.54× | 0.49× | 0.91× |
| get µs/op | 0.541 | 0.297 | 0.310 | 1.75× | 0.96× | 0.55× |
| range µs/scan | 4.968 | 0.849 | 0.334 | 14.87× | 2.54× | 0.17× |
| space MB | 3.05 | 2.50 | 5.29 | 0.58× | 0.47× | 0.82× |

### Soft async+group+cons+shr 50k

| 指标 | FCS base | FCS Post | LMDB | base/LMDB | Post/LMDB | Post/base |
|------|--------:|---------:|-----:|----------:|----------:|----------:|
| insert µs/op | 0.333 | 0.312 | 6.037 | 0.06× | 0.05× | 0.94× |
| update µs/op | 0.598 | 0.589 | 11.714 | 0.05× | 0.05× | 0.98× |
| get µs/op | 0.306 | 0.316 | 0.308 | 0.99× | 1.03× | 1.03× |
| range µs/scan | 0.721 | 1.016 | 0.324 | 2.23× | 3.14× | 1.41× |
| space MB | 2.14 | 2.15 | 5.29 | 0.41× | 0.41× | 1.00× |

## 读法

- **Post/base**：写路径应≈1.0（坑 150）；range/space 看 repay 后。
- **FCS/对手**：相对 SQLite/LMDB；&lt;1 写/读更快。
- 单 run vs LMDB ~2× = 坑 145 地板；**坑 151：Soft 默认已升 Post**（读前 repay）。
