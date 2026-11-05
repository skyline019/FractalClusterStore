# VERDICT_FLUSH_TIP（坑 159 → 163）

Soft sync Post 50k · 中位 · 5 轮 · **bloom 跳首写 settle**（163：不再用 tip.osz）

| metric | tip=on | tip=off | on/off |
|--------|-------:|--------:|-------:|
| insert | 5.246 | 6.333 | 0.83× |
| update | 6.366 | 6.801 | 0.94× |
| get | 0.490 | 0.522 | 0.94× |
| range | 0.676 | 0.687 | 0.98× |
| excl_ins | 11352 | 10931 | 1.04× |
| settle_ins | 1578 | 1780 | 0.89× |
| excl_upd | 28110 | 27174 | 1.03× |
| settle_upd | 20233 | 19925 | 1.02× |

settle_ins on/off=0.887× settle_upd=1.015× insert=0.828× update=0.936×

## VERDICT: **KEEP flush_tip default on**

工作分布见 WORK_DIST.md（Soft sync 墙钟≈97% 在 commit，phase2 I/O 为主）

复现: `bash scripts/ablate_flush_tip.sh`
