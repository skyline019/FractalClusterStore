# Soft sync 工作节奏（坑 148）

> 问题：sync 路径上**工作高峰在哪、低谷在哪、分布节奏如何**。  
> 探针：`scripts/sync_work_trace.sh` → `backup/sync-work/n{N}/`  
> 标签：相对本跑分位 — PEAK≥p90 · BUSY≥p50 · WARM≥p25 · VALLEY&lt;p25。

## 画像（Soft `configure_deploy(false)` · B=500）

### 50k（153 commits）

| 指标 | 值 |
|------|-----|
| commit_us | p50 **6.6 ms** · p90 **27 ms** · max **73 ms** @c130 upd |
| PEAK / VALLEY | 11% / 25% |
| 最长峰串 | 5（ins c64–） |
| 最长谷串 | 5（ins c97–） |
| 峰内 level 占比 | **0.66**（层合并吃峰墙钟） |

节奏：Q2/Q4 平均 commit 抬高；峰多在 **ins 中段合并浪** 与 **upd 尾**。谷在 ins 间歇与 fin。

### 200k（603 commits）— 主结论

| 指标 | 值 |
|------|-----|
| commit_us | p50 **13 ms** · p90 **63 ms** · max **249 ms** @c569 upd |
| PEAK / VALLEY | 10% / 25% |
| 最长峰串 | **20**（upd c543–，连续高压） |
| 最长谷串 | **7**（ins 前中段多段） |
| Q1→Q4 avg_us | 16 → 14 → 21 → **61 ms** |
| phase | ins avg 16 ms · peak_share 2%；**upd avg 54 ms · peak_share 27%** |
| 峰内 level 占比 | **0.44** |

```
ins 前中：VALLEY 串常见（低谷 / 暂时空闲）
ins 后 / upd：BUSY 增多
upd 末段 Q4：PEAK 扎堆（工作高峰 + 最长峰串）
fin：近空闲（~25 µs）
```

**工作高峰** = 多为 `d_level_us` 大的提交（层合并），常叠 `d_units`；空间 `peak_tail` 与最大 commit 同落在 upd 末（c569）。  
**工作低谷** = ins 阶段连续多提交 commit_us&lt;p25、level 近 0；非整段休眠，而是「轻维护 + flush」节奏。

## 对动态整理的含义

- 写中 duty-cycle 容易撞上 **upd Q4 峰串**，与 [`DISCUSS_RANGE_DYNAMIC`](DISCUSS_RANGE_DYNAMIC.md)「写中整理伤 range/税」同向。  
- **低谷窗口**更适合补做：ins 前中 valley 串（最长~7 commit），或 **fin 后**（自然空闲）。  
- post/polish 落在 fin 之后，避开 sync 峰串。

## 图

```bash
python3 scripts/plot_sync_work.py
# → backup/sync-work/n50000|n200000/work_rhythm.svg|.html
```

打开 `work_rhythm.html`：① commit 墙钟（色=VALLEY/WARM/BUSY/PEAK）② `d_level_us` ③ runs + peak_tail；灰虚线 = upd 开始。

## 复现

```bash
bash scripts/sync_work_trace.sh
python3 scripts/plot_sync_work.py
# backup/sync-work/n50000|n200000/{work_timeline.csv,work_rhythm.txt,svg,html}
```
