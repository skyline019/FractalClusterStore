# Soft A1 · P2 洞饿根源刀消融（坑 138）

> 命令：`make soft-a1-p2`  
> 配置：`configure_deploy` Soft；旋钮默认关；argv9=`starve_fit` argv10=`starve_promote`  
> 跑次：2026-11-04 · 同进程四臂

## 动机（frag 分布 → 几何根）

n200k frag：starve 时 maxh≈29–51，PEND 常 8×41。  
`part_cap_of(16)=56` ⇒ 碎片路径全拒 ⇒ 中等产物整段落尾 +311。  
α = 紧余量 `part_cap_tight(p)=p+max(p/16,4)+4`（p=32→40）；β = 封口后插 `commit_delta` 放行 PEND。

## Soft A1 表

| arm | fit | prom | peak_tail | peak_x | Δ% vs off | HOLE_STARVE | starve_fits | starve_promotes |
|-----|-----|------|-----------|--------|-----------|-------------|-------------|-----------------|
| off | 0 | 0 | 3354 | **1.802** | — | 12 | 0 | 0 |
| fit | 1 | 0 | 3643 | **1.979** | **+9.8%** | 8 | 29 | 0 |
| prom | 0 | 1 | 3354 | 1.802 | 0 | 12 | 0 | **0** |
| both | 1 | 1 | **3104** | **1.653** | **−8.3%** | 7 | 9 | 18 |

## 解读

1. **fit 单开有害**：切碎后 L2 暴涨（~68 条），末波 +608 / pend=1149 抬峰。α 几何「能切」≠ 峰优。  
2. **prom 单开无效**：无分片则很少走 `need_res` 封口等待 ⇒ `starve_promotes=0`。β 依附 α 创造的跨提交片。  
3. **both 同向降峰**：fit 切片 + promote 复用源前缀；peak_x 1.802→1.653；STARVE 12→7。  
4. **Soft 默认仍关**：A2/A3/延迟门见 `VERDICT_P2_GATE.md`（A2/A3 空间同向或平，update +16% 变慢）。实验臂 = fit+promote。

## 门控

| 候选 | 结论 |
|------|------|
| starve_fit  alone | **否**（抬峰） |
| starve_promote alone | **空操作**（本画像） |
| fit+promote | Soft A1 峰↓；A2↓/平；A3 平；**update 税 ⇒ 勿升 Soft** |
| configure_deploy | **不改** |

## 复现

```bash
cd embdb/x && make soft-a1-p2
# 单臂：./build/soft_a1 200000 500 32 1 0 0 0 0 1 1
```

回归：`c6` · 坑 138
