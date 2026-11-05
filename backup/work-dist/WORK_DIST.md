# Soft 工作分布调研（坑 159 · 2026-11-05）

探针：`backup/work-dist/probe_work_dist.cpp` · 节奏：`DISCUSS_SYNC_WORK_RHYTHM.md`（坑 148）

## Soft sync INSERT 50k B=500（本机一趟）

| 分量 | µs 合计 | 占 wall |
|------|--------:|--------:|
| **wall** | 242453 | 100% |
| put（MemTable） | 5874 | **2.4%** |
| **commit** | 234485 | **96.7%** |
| └ flush_excl（编码+settle+attach） | 8499 | 3.5% |
| └ maint level_us | 23356 | 9.6% |
| └ maint rebal_us | 7018 | 2.9% |
| └ **其余 ≈ phase2 wait_io+双 sync** | ~195k | **~81%** |

µs/op wall ≈ 4.85（与 peers Soft sync insert 同量级）。

## Soft sync UPDATE n/2

| 分量 | 占 wall |
|------|--------:|
| commit | 95% |
| flush_excl | **16.8%**（覆盖写 settle/probe 抬头） |
| level_us | 17.9% |

## Soft async+group INSERT（对照）

| 分量 | 占 wall |
|------|--------:|
| put | 49% |
| commit | 41% |
| µs/op | **0.43**（≈ sync 的 1/11） |

I/O 屏障从每批 durable 挪走后，CPU/put 才显形。

## 节奏（坑 148 · 已有）

- Soft sync 峰提交：`d_level_us` 占峰墙钟 **~66%**（层合并浪）。
- 谷：ins 前中连续轻提交；fin 后近空闲 → Post repay / polish 应落此处。

## 读路径（坑 158）

- Post repay 后 peers range：**rows/scan≈1**，open 占 **~83%**。
- open-fast KEEP（0.88×）；剩余 = 页 load/memcpy + 每 select 新 open。

## 优化优先级（本调研）→ 收口见 `OPT_COMPLETE.md`

| 优先级 | 轴 | 动作 | 状态 |
|--------|----|------|------|
| 1 | 经典 flush CPU（update excl 17%） | tip+bloom 免 probe | **KEEP 159b** |
| 2 | phase2 I/O ~81% sync wall | async+group；sync 双屏障保留 | **结案** |
| 3 | level 峰 10–18% | Post 错峰；勿 Eager | **结案** |
| 4 | range open / 拷页 | open-fast + page_view | **KEEP 158+160**（vs LMDB range 2.4→1.81×） |

## 非目标

- Soft 默认开 dual_mem（156 Negate-keep）
- 默减 durable 屏障次数
- 写中 duty-cycle 整理（148/149 已否）
