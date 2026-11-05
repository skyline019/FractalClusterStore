# Soft Post 完整 peers 消融（tip × dual × sync/async）vs SQLite / LMDB

比值 FCS/对手；&lt;1 = FCS 更快。空间 MB。坑 156/161/163。
复现：`bash scripts/peers_full_ab.sh` · `backup/peers-full/`

## vs SQLite

| 臂 | insert | update | get | range | space | /SQL ins | /SQL rng | /SQL sp |
|----|-------:|-------:|----:|------:|------:|---------:|---------:|--------:|
| sync tip=on dual=off (产品) | 4.967 | 6.405 | 0.346 | 0.489 | 2.50 | 0.80× | 0.28× | 1.12× |
| sync tip=off dual=off | 6.050 | 6.162 | 0.455 | 0.448 | 2.50 | 0.94× | 0.26× | 1.12× |
| sync tip=on dual=on | 5.482 | 5.644 | 0.384 | 0.411 | 2.50 | 0.91× | 0.18× | 1.12× |
| sync tip=off dual=on | 6.305 | 5.626 | 0.317 | 0.395 | 2.50 | 0.92× | 0.19× | 1.12× |
| async tip=on dual=off (产品) | 0.426 | 0.740 | 0.347 | 0.497 | 2.15 | 0.06× | 0.28× | 0.96× |
| async tip=off dual=off | 0.420 | 0.715 | 0.306 | 0.460 | 2.15 | 0.07× | 0.25× | 0.96× |
| async tip=on dual=on | 0.481 | 0.579 | 0.316 | 0.428 | 2.15 | 0.12× | 0.22× | 0.96× |
| async tip=off dual=on | 0.512 | 0.656 | 0.303 | 0.383 | 2.15 | 0.09× | 0.20× | 0.96× |

SQLite 基线（同 sync_t1d0 跑）：ins=6.210 upd=14.932 get=1.690 rng=1.767 sp=2.24

## vs LMDB

| 臂 | insert | update | get | range | space | /LMDB ins | /LMDB get | /LMDB rng | /LMDB sp |
|----|-------:|-------:|----:|------:|------:|----------:|----------:|----------:|---------:|
| sync tip=on dual=off (产品) | 5.160 | 5.172 | 0.303 | 0.370 | 2.50 | 0.89× | 0.96× | 1.09× | 0.47× |
| sync tip=off dual=off | 4.638 | 5.072 | 0.305 | 0.381 | 2.50 | 0.74× | 0.99× | 1.12× | 0.47× |
| sync tip=on dual=on | 4.614 | 4.543 | 0.299 | 0.392 | 2.50 | 0.94× | 0.92× | 1.17× | 0.47× |
| sync tip=off dual=on | 4.439 | 4.250 | 0.308 | 0.404 | 2.50 | 0.69× | 0.92× | 1.15× | 0.47× |
| async tip=on dual=off (产品) | 0.349 | 0.632 | 0.311 | 0.394 | 2.15 | 0.06× | 0.96× | 1.19× | 0.41× |
| async tip=off dual=off | 0.359 | 0.665 | 0.299 | 0.392 | 2.15 | 0.06× | 0.91× | 1.14× | 0.41× |
| async tip=on dual=on | 0.464 | 0.598 | 0.312 | 0.393 | 2.15 | 0.06× | 0.80× | 1.15× | 0.41× |
| async tip=off dual=on | 0.465 | 0.599 | 0.360 | 0.408 | 2.15 | 0.07× | 1.14× | 1.18× | 0.41× |

## 消融判决（sync）

- tip on/off（dual=off）：insert 0.82× · update 1.04× · get 0.76× · range 1.09× · space 1.00×
- dual on/off（tip=on）：insert 1.10× · update 0.88× · space 1.00×

## 产品臂（sync tip=on dual=off）

- vs SQL：ins 0.80× · upd 0.43× · get 0.20× · rng 0.28× · sp 1.12×
- vs LMDB：ins 0.89× · get 0.96× · rng 1.09× · sp 0.47×

- tip：**KEEP** 默认 on（bloom 跳首写；163 后 g16 绿）
- dual：Soft 默认仍关（Negate-keep）
