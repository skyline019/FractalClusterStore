# P2 写税归因（Soft sync 交错消融）

源：`backup/soft-a1/latency_p2_tax.csv` · median µs/op · N=50k Soft sync

| arm | insert | Δ% | update | Δ% | get | Δ% | fits | prom | parts |
|-----|-------:|---:|-------:|---:|----:|---:|-----:|-----:|------:|
| off | 4.597 | +0.0% | 5.777 | +0.0% | 0.585 | +0.0% | 0 | 0 | 0 |
| fit | 4.867 | +5.9% | 6.792 | +17.6% | 0.564 | -3.6% | 0 | 0 | 0 |
| prom | 5.212 | +13.4% | 5.807 | +0.5% | 0.493 | -15.7% | 0 | 0 | 0 |
| both | 4.901 | +6.6% | 5.869 | +1.6% | 0.558 | -4.6% | 0 | 0 | 0 |

## 归因（相对 off）

- **fit 单开**：insert +5.9% · update +17.6% · fits≈0 prom≈0
- **prom 单开**：insert +13.4% · update +0.5% · fits≈0 prom≈0
- **both**：insert +6.6% · update +1.6% · fits≈0 prom≈0

## leading

- **写税为 α+β 合成**（或噪声）：看 fits/prom/parts 与 both 是否叠加。

