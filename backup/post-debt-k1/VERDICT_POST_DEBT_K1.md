# VERDICT_POST_DEBT（坑 157）

Soft sync Post 50k · 中位 µs/op · 5 轮 · k1 已删（Negate）

| metric | cheap | snap | cheap/snap |
|--------|------:|-----:|-----------:|
| insert | 5.719 | 6.074 | 0.94× |
| update | 7.011 | 6.802 | 1.03× |
| get | 0.348 | 0.391 | 0.89× |
| range | 0.769 | 0.758 | 1.01× |

insert cheap/snap=0.942× update=1.031× get=0.890× range=1.015×

## VERDICT: **KEEP cheap Post debt (default)**

k1 knife: NEGATE（3 轮 range 无胜且 update 回归）→ 代码已删

复现: `bash scripts/ablate_post_debt_k1.sh`
