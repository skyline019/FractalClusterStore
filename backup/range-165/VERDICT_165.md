# 坑 165 · Cur::reuse + 单段跳 may_have + get 免双锁

## 根因（短窗 oneseg 曾 Negate）

`Cur{}` / `cs_[0]=Cur{}` 每次清零 **PAGE 缓冲（4KB）** ⇒ 短窗 oneseg 比复用 `scr_` 的 Cursor 更慢（曾 1.22×）。

## 刀

1. `Cur::reuse()`：只重置导航字段，不碰 `buf`
2. 单段 prime：`reuse()` + **跳过 `run_may_have` sp 二分**（键域已相交）
3. `get` → `get_at_locked_`：免双重 `RGuard`
4. `oneseg_cur_` 池：全表 `limit==0` 仍走 oneseg（短窗仍 Cursor，但受益于 1–2）

## 消融（`ablate_range_165.sh` · open_fast+oneseg_sel on vs 全关）

| pass | short on/off | full on/off | get µs |
|------|-------------:|------------:|-------:|
| 1 | **0.654** | **0.591** | 0.288 |
| 2 | **0.665** | **0.610** | 0.280 |

## Peers 产品臂（165 后抽样）

| | sync vs SQL | sync vs LMDB | async vs SQL | async vs LMDB |
|--|--:|--:|--:|--:|
| range | **0.22×** | **1.32×**（原 ~1.70） | **0.20×** | **1.16×**（原 ~1.39） |
| get | 0.20× | 1.12× | 0.22× | **0.96×** |
| insert | 0.95× | 0.91× | 0.09× | 0.06× |
| space | 1.12× | 0.47× | 0.96× | 0.41× |

## 判决

**KEEP** Soft 默认（open_fast / oneseg_sel limit=0 / tip / page_view / pf defer）。短窗 oneseg 直扫仍不升（池化后仍 ~中性）。
