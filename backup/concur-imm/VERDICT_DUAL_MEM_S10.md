# VERDICT_DUAL_MEM_S10（坑 156 · tip+osz 免 probe）

复现: `bash scripts/ablate_dual_mem_s10.sh`

## 中位

| 画像 | arm | wall | excl | enc | io | settle | get_max |
|------|-----|------|------|-----|----|--------|---------|
| write | off | 307184 | 301290 | 0 | 0 | 0 | 0 |
| write | on | 49857 | 15830 | 3606 | 8082 | 1659 | 0 |
| lock | off | 41048 | 38135 | 0 | 0 | 0 | 0 |
| lock | on | 29181 | 3891 | 5261 | 10340 | 316 | 0 |
| stall | off | 74787 | 7279 | 0 | 0 | 0 | 1287 |
| stall | on | 70259 | 2440 | 3039 | 3012 | 65 | 1165 |
| mw | off | 447781 | 145659 | 0 | 0 | 0 | 3298 |
| mw | on | 325836 | 56836 | 8567 | 16031 | 30033 | 2275 |
| syncstall | off | 654454 | 1274 | 0 | 0 | 0 | 14 |
| syncstall | on | 651165 | 1370 | 1038 | 136 | 29 | 10 |

## 门禁

| 门 | 实测 | 过？ |
|----|------|------|
| lock excl ≤0.5× | 3891/38135=0.10× | PASS |
| lock wall ≤1.15× | 0.71× | PASS |
| write wall ≤0.50× | 0.16× | PASS |
| write excl ≤0.15× | 0.05× | PASS |
| settle ≤0.02× off_excl | 0.01× | PASS |
| stall get_max | off=1287 on=1165 | PASS |
| mw wall | 0.73× | PASS |
| mw get_max | off=3298 on=2275 | PASS |
| syncstall get_max | off=14 on=10 | PASS |

## 判决：**Negate-keep**

S10 主门全过（settle 免 tip 页探；write wall≤0.50×）；仍缺生产长稳 ⇒ Soft 默关。

S10：`DualTip{rid,len,osz}` 写出时记下；settle 命中免 probe；NIL+primed 跳过；merge bump gen → full_probe。
