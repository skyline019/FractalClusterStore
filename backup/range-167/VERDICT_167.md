# 坑 167：短窗地板诊断 + fences/seek load 消融

## 剖面（Post repay · peers 同构）

| 观测 | 值 |
|------|-----|
| runs / cells | 1 / 100 |
| **emit_avg**（K=100 · win=4096） | **1.00**（短窗≈点查） |
| open / first_next / rest | ~0.21 / ~0.15 / ~0.08 µs |
| cache≥512 | hit_rate≈1.0（DB ~640 页；2048 槽足够） |
| hot_same_lo / random select | ~0.21 / ~0.45（局部性，非 knobs） |
| seq / rand select | ~0.63× |

结论：**剩余缝主要是随机首页 touch + Cursor/RGuard 壳**；非 k-way、非 cache 容量、非 tip/dual。

## 刀：`Cur::load` fences 定页 + 首填 seek ≥ from

配对消融（同 key on→off，无缓存偏置校正后仍稳）：

| | open | first | 合计 o/f |
|--|--:|--:|--:|
| fence+seek on | 0.371 | 0.047 | |
| off（旧 sp） | 0.205 | 0.148 | |
| paired mean | | | **1.29×** |

Oracle 正确，但 **Negate**（把 skip 挪进 open 的税 > first 节省）。代码已回滚；**不留旋钮**。

## 旁路

| 尝试 | 结果 |
|------|------|
| 短窗 oneseg 再开 | 仍 ~1.0×（166 Negate） |
| set_prefetch fair 中位 | on/off ~0.94（无稳胜；勿改 Soft） |
| 加大 cache | ≥512 已满命中；非刀 |

## 产品含义

- peers sync range/LMDB **~1.3× 中位**（见 `PEERS_FULL_MED.md`）与「1 行窗 + 非 mmap」地板一致
- 下一刀需换轴：应用侧 Cursor 复用 / mmap 类设备 / 或接受地板；勿再堆无消融胜的 load 微调
