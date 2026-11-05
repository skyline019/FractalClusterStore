# VERDICT_DUAL_MEM（坑 156 · 汇总）

最新：**S10** → [`VERDICT_DUAL_MEM_S10.md`](VERDICT_DUAL_MEM_S10.md)

## 判决：**Negate-keep**

门禁已全过；Soft / `configure_deploy` **仍不得**默认 on（待生产长稳）。

| 代 | lock excl | write wall | settle÷off_excl | mw wall | 要点 |
|----|-----------|------------|-----------------|---------|------|
| S7 | 0.06× | 1.01× | ~1 | 0.95× | settle 出独占 |
| S8 | 0.05× | 1.01× | ~1 | 0.77× | 异步 settle |
| S9 | 0.09× | **0.16×** | 0.05× | 0.68× | tip 跳过空扫 |
| **S10** | **~0.1×** | **~0.16×** | **~0.005×** | **~0.6×** | tip+osz **免 probe** |

S10：`DualTip{rid,len,osz}` 写出时记下；settle 命中不再读页。
