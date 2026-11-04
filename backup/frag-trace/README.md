# Frag 时空探针（坑 136）

```bash
make frag-trace
# 或：build/frag_trace [N] [B] [VSZ] [out_dir] [bf_mode] [bf_skip]
# 大盘：build/frag_trace 200000 500 32 backup/frag-trace 0 0
```

| 文件 | 内容 |
|------|------|
| `timeline.csv` | **时序**：每提交一行——洞数/maxh/≥64 计数、free 直方图桶、PEND/DEF、层、part_frags、ATTR、event |
| `holes.csv` | **空间**：事件点（STARVE/PEAK/FRAG_BUMP/…）每条 FREE/PEND/DEF 的 `(p0,np,mid)` |
| `summary.txt` | 峰/末态/事件计数 |

`event=1` 行 = 该提交触发了空间全量转储。分析例：按 `commit` 看 `n_ge64`/`maxh`；在 `holes.csv` 滤 `tag=PEAK` 看峰时地址分布。
