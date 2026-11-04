# Soft A1 写税中位（坑 139 · STARVE_FLOOR=32 MAX_SLICES=12 MAX_PROMOTES=1 · ×3）

| arm | med peak_x | med insert | Δ% | med update | Δ% |
|-----|----------:|-----------:|---:|-----------:|---:|
| off | 1.811 | 27.719 | 0 | 102.323 | 0 |
| both | 1.586 | 30.462 | +9.9% | 94.447 | -7.7% |

## raw
```
rep,arm,peak_x,peak_tail,insert_us,update_us,fits,prom,frags
1,off,1.811,3353,29.407,102.323,0,0,5
1,both,1.586,2987,30.928,94.447,4,1,14
2,off,1.787,3324,27.393,97.146,0,0,5
2,both,1.607,3004,30.100,102.342,7,1,16
3,off,1.915,3512,27.719,133.947,0,0,7
3,both,1.549,2911,30.462,93.083,4,1,15

```

## leading

- peak med 1.811→1.586 (-12.4%)
- insert med Δ +9.9%（税刀前单次 both≈+39%）
- update med Δ -7.7%
- Soft 默认仍关；A2/A3/sync 门禁未在本轮重跑则 KPI_UNVERIFIED。
