# VERDICT_DUAL_MEM_S8（坑 156 · 异步 settle）

复现: `bash scripts/ablate_dual_mem_s8.sh`

## 中位

| 画像 | arm | wall | excl | enc | io | settle | get_max |
|------|-----|------|------|-----|----|--------|---------|
| write | off | 307203 | 302123 | 0 | 0 | 0 | 0 |
| write | on | 311308 | 5041 | 3520 | 7772 | 281251 | 0 |
| lock | off | 39914 | 36272 | 0 | 0 | 0 | 0 |
| lock | on | 37881 | 1668 | 4560 | 18169 | 24417 | 0 |
| stall | off | 64892 | 7455 | 0 | 0 | 0 | 1172 |
| stall | on | 74343 | 838 | 1781 | 2735 | 203 | 443 |
| mw | off | 441135 | 155787 | 0 | 0 | 0 | 4101 |
| mw | on | 339328 | 58724 | 7019 | 11735 | 90758 | 3516 |
| syncstall | off | 649390 | 1027 | 0 | 0 | 0 | 10 |
| syncstall | on | 651835 | 221 | 802 | 176 | 29 | 9 |

## 门禁

| 门 | 实测 | 过？ |
|----|------|------|
| lock excl ≤0.5× | 1668/36272=0.05× | PASS |
| write wall ≤1.10× | 1.01× | PASS |
| write excl ≤0.15× | 0.02× | PASS |
| stall get_max | off=1172 on=443 | PASS |
| mw wall | 0.77× | PASS |
| mw get_max | off=4101 on=3516 | PASS |
| syncstall get_max | off=10 on=9 | PASS |

## 判决：**Negate-keep**

S8 主门全过（excl≤0.15×；wall≤1.10× RamDev 仍受 settle CPU 限制）；缺生产长稳 ⇒ Soft 默关。

S8：先 settle_mu_ 再 XLock attach（禁止持独占等 settle）；异步死账与 phase1 尾/puts 重叠；RamDev wall≈持平因 settle CPU≫phase2。
