# VERDICT_DUAL_MEM_S7（坑 156 · settle 出独占）

复现: `bash scripts/ablate_dual_mem_s7.sh`

## 中位

| 画像 | arm | wall | excl | enc | io | settle | get_max |
|------|-----|------|------|-----|----|--------|---------|
| write | off | 300930 | 294676 | 0 | 0 | 0 | 0 |
| write | on | 304183 | 6046 | 3809 | 6910 | 281784 | 0 |
| lock | off | 42520 | 39255 | 0 | 0 | 0 | 0 |
| lock | on | 42896 | 2518 | 11011 | 14325 | 9597 | 0 |
| stall | off | 68771 | 7986 | 0 | 0 | 0 | 919 |
| stall | on | 62895 | 1667 | 2602 | 3725 | 260 | 961 |
| mw | off | 466747 | 162673 | 0 | 0 | 0 | 3665 |
| mw | on | 441877 | 42904 | 7686 | 11809 | 84831 | 2847 |
| syncstall | off | 678375 | 1592 | 0 | 0 | 0 | 12 |
| syncstall | on | 649764 | 877 | 822 | 95 | 25 | 11 |

## 门禁

| 门 | 实测 | 过？ |
|----|------|------|
| lock excl ≤0.5× | 2518/39255=0.06× | PASS |
| write wall ≤1.10× | 1.01× | PASS |
| write excl on/off | 0.02× | (观测) |
| stall get_max | off=919 on=961 | PASS |
| mw wall | 0.95× | PASS |
| mw get_max | off=3665 on=2847 | PASS |
| syncstall get_max | off=12 on=11 | PASS |

## 判决：**Negate-keep**

S7 主门全过（含 syncstall 绝对/放宽相对）；仍缺生产多负载长稳 ⇒ Soft 默关。

S7：XLock=切表+attach；共享锁+settle_mu_=probe_prev 死账；读者可与 settle 并行。
