# 坑 164 · 空 mem 单段 select / prime / get

## 瓶颈

peers Soft Post repay 后 open 仍占 select ~65%；vs LMDB range ~1.45×、get ~1.18×。

## 刀

1. **`range_oneseg_sel`**（Soft 开）：`mem` 空且 `limit==0` 时直扫 `Cur`，跳过 Cursor 归并。  
   - 短窗（limit>0）**不走**（曾测 short on/off **1.22×** 更慢 —— Negate 短窗）。
2. **`range_open_fast` 单段 prime 复用 `cs_[0]`**（164b）：免 clear/reserve/计数循环。
3. **get 单段跳过 bloom**：Soft repay 单段必 probe；省位测。

## 消融

| 臂 | on/off | 判决 |
|----|-------:|------|
| full select (`limit=0`) oneseg | **0.57–0.62×** | **KEEP** |
| short peers 窗 oneseg（曾全开） | 1.22× | **Negate**（现仅 limit=0） |
| prime open_fast（含 164b） | **0.86×** | **KEEP**（原 158 加强） |
| get oneseg | ≈中性同库；peers get/LMDB **1.01×** | **KEEP** |

Oracle：`oracle164.cpp` full/short 计数与校验和一致。

## 复现

```bash
bash scripts/ablate_range_oneseg_sel.sh
g++ -O2 -std=c++17 -I. backup/range-oneseg-sel/oracle164.cpp -o /tmp/oracle164 -lpthread && /tmp/oracle164
```
