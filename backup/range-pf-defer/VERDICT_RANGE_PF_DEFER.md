# 坑 161 · range 翻页才预取 VERDICT

## 假说

Post repay 后 peers range 常 **1 行/窗**；`Cur::fill` 首填仍 `prefetch(PF=8)` ⇒ 白烧缓存/页槽。  
竞品：prefetch 关更快是噪声；或 defer 伤全表扫。

## 刀

- `range_pf_eager_=false`（Soft `configure_deploy`）：仅 `fixup` 翻页时预取；首填不预取。
- 消融：`set_range_pf_eager(true)` 恢复每 fill 预取。

## 同库交叉（50k Soft Post+page_view · `ablate_range_pf_defer.sh`）

| pass | short defer/eager | full defer/eager |
|------|------------------:|-----------------:|
| 1 | **0.732** | 1.036 |
| 2 | **0.856** | 0.930 |

short = peers 窗 `lo..+4096` ×K=100（实测 rows≈1）；full = 全键域扫 ×8。

## 判决

**KEEP** Soft 默认 defer（eager off）。短扫稳赢；全表扫 ≈中性（噪声带）。

## Peers

同库 short 胜稳；peers 绝对值受噪声/对手抖动。以 `ablate_range_pf_defer.sh` 为准。见 `PEERS_DUAL.md` · `OPT_COMPLETE.md`。
