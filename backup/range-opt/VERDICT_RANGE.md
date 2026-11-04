# Soft range 读刀（坑 145）

## 缝（peers，已有）

Soft sync 50k range ≈ **4 µs** vs LMDB ≈ **0.31 µs**（~12–14×）。  
Soft async+group+**cons+shr** ≈ **0.7 µs**（~2×）。缝在**未整理多 run**，不是 k 路归并微优化。

## 结构（本轮）

| 态 | runs | cursors/scan | range（本机噪声大） |
|----|-----:|-------------:|---------------------|
| Soft live 50k | ~10 | ~12.4 | ≫ polish |
| Soft+polish 50k | 1 | ~1.25 | ~4 µs 量级 |
| Soft 200k P2 | ~30 | ~6.5 | 同量级 |

`cursors/scan` = 跨 cell 累加的 Cur 打开数；Soft live 宽≈ run 相交数。

## 消融

| 刀 | 结果 | 处置 |
|----|------|------|
| `cheap_pages` 1024→64 | Soft runs 不变（仍≈10） | 不改默认 |
| `lvl_t=4` | runs↓ 但 cursors↑、range 更差 | 不改默认（EVAL17 T1） |
| `lvl_cliff=2` | 无差 | 不改（CONSTRAINTS 拒 cliff 默认回潮） |
| k≥4 heap | 先前无增益 | 已撤 |
| **家族链** `FCS_RANGE_FAMILY_CHAIN` | Soft 50k/200k cursors 持平（同 seq 多片未打进窗口） | **保留**（与 merge 同口径；P2 片共窗时防 k≈片数） |
| k=1 快路径 | polish 臂噪声内无稳定胜 | **不进树** |
| Soft 默更猛合并 | 违反「写换读 + polish」边界 | 不做 |

## 结论

- **产品读法不变**：未整理 Soft sync range 慢 = 写换读；对外读服务前 `polish` / cons+shr。
- **代码落地**：`Cursor::prime` 家族链 + k≤2 预取；微优化刀全部消融掉。
- **KPI**：本轮未重跑 peers；peers 数字仍以 `PEERS_P2.md` 为准。

复现：`scripts/ablate_range_cheap.sh` · `scripts/ablate_range_k1.sh` · `scripts/ablate_range_chain_p2.sh` · `scripts/range_knives.sh`
