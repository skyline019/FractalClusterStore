# VERDICT_DUAL_MEM_S6（坑 156 · finish 出锁 + 多写者）

复现：`bash scripts/ablate_dual_mem_s6.sh`

## 中位对照

| 画像 | arm | wall | flush_excl | encode | io | flushes | get_max |
|------|-----|------|------------|--------|----|---------|---------|
| write | off | 224380 | 217886 | 0 | 0 | 0 | 0 |
| write | on | 227480 | 209921 | 3653 | 8116 | 250 | 0 |
| lock | off | 46825 | 43673 | 0 | 0 | 0 | 0 |
| lock | on | 49264 | 11047 | 12568 | 21632 | 40 | 0 |
| stall | off | 64698 | 6015 | 0 | 0 | 0 | 700 |
| stall | on | 62026 | 1682 | 3514 | 3609 | 21 | 1022 |
| mw | off | 454991 | 122928 | 0 | 0 | 0 | 3066 |
| mw | on | 406039 | 116308 | 10589 | 15757 | 185 | 3777 |

## 门禁

| 门 | 实测 | 过？ |
|----|------|------|
| 锁内 excl ≤0.5× | 11047/43673=0.25× | PASS |
| write wall ≤1.10× | 1.01× | PASS |
| stall get_max | off=700 on=1022 | FAIL |
| mw×3 wall ≤1.10× | 0.89× | PASS |
| mw get_max | off=3066 on=3777 | FAIL |

## 判决：**Negate-keep**

lock_ok=True write_ok=True stall_ok=False mw_wall_ok=True mw_get_ok=False；不升 Soft。

S6 机制：锁外 encode + finish_run（`dual_mu_` 串行 free-list）；锁内仅 attach/账。
Soft 默认仍关。
