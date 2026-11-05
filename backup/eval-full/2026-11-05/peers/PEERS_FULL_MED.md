# Soft Post 完整 peers 消融（中位 ×5）vs SQLite / LMDB

比值 FCS/对手；<1 = FCS 更快。空间 MB。坑 163/165/166。
复现：`R=5 bash scripts/peers_full_median.sh` · `backup/eval-full/2026-11-05/peers/`

## vs SQLite（中位）

| 臂 | insert | update | get | range | space | /SQL ins | /SQL upd | /SQL get | /SQL rng | /SQL sp |
|----|-------:|-------:|----:|------:|------:|---------:|---------:|---------:|---------:|--------:|
| sync tip=on dual=off (产品) | 5.397 | 6.575 | 0.420 | 0.477 | 2.50 | 0.99× | 0.62× | 0.24× | 0.26× | 1.12× |
| sync tip=off dual=off | 6.101 | 6.044 | 0.375 | 0.506 | 2.50 | 1.11× | 0.46× | 0.21× | 0.27× | 1.12× |
| sync tip=on dual=on | 6.114 | 5.474 | 0.377 | 0.447 | 2.50 | 1.22× | 0.43× | 0.21× | 0.23× | 1.12× |
| async tip=on dual=off (产品) | 0.450 | 0.649 | 0.345 | 0.402 | 2.15 | 0.09× | 0.07× | 0.20× | 0.22× | 0.96× |

## vs LMDB（中位）

| 臂 | insert | update | get | range | space | /LMDB ins | /LMDB upd | /LMDB get | /LMDB rng | /LMDB sp |
|----|-------:|-------:|----:|------:|------:|----------:|----------:|----------:|----------:|---------:|
| sync tip=on dual=off (产品) | 6.303 | 7.139 | 0.351 | 0.395 | 2.50 | 0.94× | 0.54× | 1.04× | 1.11× | 0.47× |
| sync tip=off dual=off | 6.101 | 7.349 | 0.328 | 0.403 | 2.50 | 0.99× | 0.64× | 1.02× | 1.15× | 0.47× |
| sync tip=on dual=on | 5.918 | 5.592 | 0.408 | 0.453 | 2.50 | 0.86× | 0.43× | 1.23× | 1.28× | 0.47× |
| async tip=on dual=off (产品) | 0.462 | 0.686 | 0.338 | 0.411 | 2.15 | 0.07× | 0.06× | 1.01× | 1.19× | 0.41× |

## 消融判决（sync 中位）

- tip on/off（dual=off）：insert 0.88× · update 1.09× · get 1.12× · range 0.94× · space 1.00×
- dual on/off（tip=on）：insert 1.13× · update 0.83× · get 0.90× · range 0.94× · space 1.00×

## 产品臂（中位）

### sync tip=on dual=off
- vs SQL：ins 0.99× · upd 0.62× · get 0.24× · rng 0.26× · sp 1.12×
- vs LMDB：ins 0.94× · upd 0.54× · get 1.04× · rng 1.11× · sp 0.47×

### async tip=on dual=off
- vs SQL：ins 0.09× · upd 0.07× · get 0.20× · rng 0.22× · sp 0.96×
- vs LMDB：ins 0.07× · upd 0.06× · get 1.01× · rng 1.19× · sp 0.41×

- tip：**KEEP**（163 bloom；中位 insert 相对 tip-off 无稳定回归才维持；见上 tip 行）
- dual：Soft 默认仍关（Negate-keep）
