# 深度：哪些页 alive / dead（Soft A1 峰）

> 探针：`./build/soft_a1 200000 500 32 1 1` → `alive_dead.log`  
> 口径：引擎**无 per-page live bit**；数据页按段键域扫记录，用 `newer_has` / `range_hidden` 判定（与 `exact_live_recs` 同构）。

## 1. 引擎粒度（先于数字）

| 粒度 | 有无 alive 账 | 含义 |
|------|----------------|------|
| **记录** | 有（归并赢家） | 键的最新可见副本 |
| **页 `PK_DATA`** | **无** | 只存物理 `used`/`pnrec`；死记录仍躺在页里直到 TRIM/退休/重写 |
| **段 Seg** | `Seg.live` | 本单元内该段胜出条数 |
| **Run** | `live_recs` / `live_bytes` | 全库该 run 仍为最新的条数；0 ⇒ 整 run 可零写退休 |
| **free_ / pending_free_ / deferred_** | 页所有权队列 | **不是**「页内死活」 |

覆盖写：新 run 落新页；旧页里的旧副本变死记录，**页仍整页算在 run 块里**。

## 2. Soft A1 峰时刻存货（commit 569，tail=3427）

```
pages: FREE=536 PEND=987 DEF=0 MAN=25 OVFL=0 META=120 ANCHOR=0
       DATA_LIVE=605 DATA_MIX=1152 DATA_DEAD=0 UNCLAIMED=0
records: nrec=193580 exact_live=187995 dead=5585 dead_frac=0.029
reclaim_gate: DATA_DEAD+ANCHOR=0；MIX=1152；FREE+PEND=0.444·tail
```

**读法：**

| 类 | 页数 | 占峰 tail | 是什么 |
|----|------|-----------|--------|
| **PEND** | 987 | 29% | 已从 run 摘下、等 SB 才进 `free_` —— **峰的最大「假占用」** |
| **FREE** | 536 | 16% | 已可分配的洞（养洞失败时产物仍长尾） |
| **DATA_MIX** | 1152 | 34% | 页内活+死记录混杂；**整页仍属 run** |
| **DATA_LIVE** | 605 | 18% | 扫到的记录全活 |
| **DATA_DEAD** | **0** | 0 | **没有整页全死的数据页** |
| META+MAN | 145 | 4% | 元数据 / 清单 |

记录级 dead 仅 **2.9%**；页级「全死」为 **0**。  
脏 run 全是 `pg_mix`（例：L3 `250` 91 页全 MIX，live 9395/10088）。

## 3. 末态（tail=2692）对照

```
FREE=677 PEND=0 … DATA_LIVE=44 DATA_MIX=1798 DATA_DEAD=0
records dead_frac=0.080
```

- `pending` 已 promote ⇒ 峰里那 987 页回落进洞/复用。  
- 仍 **0** 全死数据页；MIX 更多（覆盖写沉淀）。  
- 末态峰比下降靠 **PEND 消失 + hw_reclaim**，不是挖出全死页。

## 4. 结论（对刀口）

1. **峰不是「一堆全死页」**，是 **PEND 隔离 + 洞装不下产物 + MIX 页整页占用**。  
2. **P2c（页内挖洞）**：峰时 dead_frac≈3%、DATA_DEAD=0 ⇒ 挖洞收益薄；MIX 多但死字节少。**勿用 P2c 当 A1 主刀。**  
3. **下一刀仍应对准养洞/分配**：让 FREE/PEND 能接住分片产物，而不是清页内死记录。  
4. 旧 `dead_est≈(1-live/nrec)*npages` 在峰时会低估「PEND 假峰」、高估「可回收死页」——以本清单为准。

## 5. 复现

```bash
cd embdb/x && ./build/soft_a1 200000 500 32 1 1 | tee backup/soft-a1/alive_dead.log
# 看 == LIVE_INV PEAK_c* 与 == LIVE_INV END
```
