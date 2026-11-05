# Soft 完整优化收口（坑 157–166）

## 旋钮（产品 Soft）

```cpp
soft_cons = Post             // 151
flush_tip = true             // 163 KEEP（bloom 跳首写；勿 tip.osz）
range_open_fast = true       // 158 + 164b/165 reuse + 跳 may_have
range_page_view = true       // 160
range_pf_eager = false       // 161 翻页才预取
range_oneseg_sel = true      // 164/165/171：仅 limit==0；oneseg 内无 limit 截断枝
dual_mem = false             // 156 Negate-keep（收敛期保留 API）
// 166：Cursor::next 单 Cur 快路径（无旋钮；短窗走此路）
```

## 坑 166 / 171

| 刀 | 证据 | 判决 |
|----|------|------|
| Cursor 空 mem+单 Cur `next` | sync range/LMDB **1.09×** | **KEEP** |
| 短窗 oneseg 再开 | median ~1.0× | **Negate**；171 删 oneseg `limit` 死枝 |
| 去掉 oneseg 预扫 | 已吐行截断风险 | **勿删** |
| dual_mem / space_accel / P1* | Soft 勿升；消融测仍用 | **保留 API**（171） |

## Peers 产品臂（全量评 2026-11-05 · ×5 比值中位）

| | sync vs SQL | sync vs LMDB | async vs SQL | async vs LMDB |
|--|--:|--:|--:|--:|
| insert | **0.96×** | **0.95×** | **0.10×** | **0.07×** |
| update | **0.63×** | **0.59×** | **0.07×** | **0.07×** |
| get | **0.24×** | **0.97×** | **0.20×** | 1.06× |
| range | **0.26×** | **1.18×** | **0.22×** | **1.17×** |
| space | 1.12× | **0.47×** | 0.96× | **0.41×** |

tip KEEP；dual Negate-keep；Soft A1 peak_x **1.84**。源：`backup/eval-full/2026-11-05/EVAL_FULL.md`

## 剩余地板（坑 167 剖面）

- peers 短窗 **emit_avg=1.0**（K=100 窗仍≈1 行）；cache≥512 已满命中
- sync range/LMDB **~1.31× 中位**：随机首页 touch + Cursor/RGuard 壳 vs LMDB mmap
- fences+seek load **Negate 1.29×**（已回滚）；勿再堆同类微调
- sync durable：phase2 双屏障 → async 吞吐轴
- 勿恢复 tip.osz / 短窗 oneseg / Cur{} 清 PAGE / fence-seek load
