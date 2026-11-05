# VERDICT_DUAL_MEM_S9（坑 156 · tip settle）

复现: `bash scripts/ablate_dual_mem_s9.sh`

## 中位

| 画像 | arm | wall | excl | enc | io | settle | get_max |
|------|-----|------|------|-----|----|--------|---------|
| write | off | 295145 | 289048 | 0 | 0 | 0 | 0 |
| write | on | 47854 | 12940 | 4441 | 9610 | 13326 | 0 |
| lock | off | 33581 | 30700 | 0 | 0 | 0 | 0 |
| lock | on | 45205 | 2821 | 10381 | 17148 | 9897 | 0 |
| stall | off | 60781 | 5798 | 0 | 0 | 0 | 1499 |
| stall | on | 76528 | 1171 | 2961 | 3429 | 58 | 863 |
| mw | off | 455627 | 151605 | 0 | 0 | 0 | 4331 |
| mw | on | 311228 | 44228 | 7488 | 12513 | 22387 | 3179 |
| syncstall | off | 655773 | 631 | 0 | 0 | 0 | 9 |
| syncstall | on | 655299 | 466 | 1285 | 127 | 29 | 11 |

## 门禁

| 门 | 实测 | 过？ |
|----|------|------|
| lock excl ≤0.5× | 2821/30700=0.09× | PASS |
| write wall ≤0.50× | 0.16× | PASS |
| write excl ≤0.15× | 0.04× | PASS |
| settle ≤0.20× off_excl | 0.05× | PASS |
| stall get_max | off=1499 on=863 | PASS |
| mw wall | 0.68× | PASS |
| mw get_max | off=4331 on=3179 | PASS |
| syncstall get_max | off=9 on=11 | PASS |

## 判决：**Negate-keep**

S9 主门全过（write wall≤0.50× · settle≪旧 excl）；仍缺生产长稳 ⇒ Soft 默关。

S9：`dual_tip_` 记住键的上次 mem-flush rid；settle 直探一 run；tip=NIL 且已 primed ⇒ 跳过（首次写入）。merge bump gen 后下一批 full_probe。
