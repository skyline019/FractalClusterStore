# 坑 166：Cursor 单 Cur next 快路径 + 短窗 oneseg 再消融

## 刀

1. **`Cursor::next`**：空 mem + `cs_.size()==1` ⇒ 直取 run 行，免 k 路/mem 归并壳（Post repay 后 peers 主路径）。
2. **短窗 oneseg 再开**：`select` 放开 `limit>0` 走 `select_oneseg_`（仍 `oneseg_cur_.reuse` + 跳 `run_may_have`）。
3. 预扫 `segs.size()==1` **保留**（去掉会在已吐行后遇多段时静默截断）。

## 消融（`scripts/ablate_range_166.sh`）

| | pass1 median | pass2 median |
|--|--:|--:|
| short oneseg on/off | **0.979×** | **1.014×** |
| full oneseg on/off | 0.619× | 0.563× |
| oracle | OK | OK |

短窗 oneseg ≈中性 ⇒ **仍 Negate**（CONSTRAINTS：仅 `limit==0`）。全表 oneseg 维持 KEEP。

## Peers 产品臂（Cursor next 快路径 KEEP）

| | sync vs SQL | sync vs LMDB | async vs SQL | async vs LMDB |
|--|--:|--:|--:|--:|
| insert | 0.80× | 0.89× | 0.06× | 0.06× |
| get | 0.20× | **0.96×** | 0.20× | 0.96× |
| range | 0.28× | **1.09×** | 0.28× | 1.19× |
| space | 1.12× | 0.47× | 0.96× | 0.41× |

相对坑 165：sync range/LMDB **1.32→1.09×**；sync get/LMDB **1.12→0.96×**。

源：`backup/peers-full/*_166.log` · `PEERS_FULL_166.md` · `ci-smoke` 绿。

## 判决

| 项 | 判决 |
|----|------|
| Cursor 单 Cur `next` 快路径 | **KEEP**（无新旋钮） |
| 短窗 oneseg | **Negate**（~1.0×；勿改 `limit==0` 门） |
| 全表 oneseg | KEEP（164/165） |
