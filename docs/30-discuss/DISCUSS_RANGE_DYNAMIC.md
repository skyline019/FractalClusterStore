# 动态整理探讨（坑 147 续）

> 目标：用**限速 / 滞后 / 写后补做**改善 eager 写税与峰（当时未升 Soft 默认）。  
> 探针：`scripts/ablate_range_dyn.sh` → `backup/range-opt/root/dyn_tidy.*`  
> 后续：写后语义落地为 **SoftCons::Post**（坑 150/151 已升 Soft 默认）。本文保留 duty/hyst **否定**证据。

## 1. 策略定义（应用侧，引擎默认不变）

| arm | 行为 |
|-----|------|
| soft | Soft，无 tidy |
| eager | 每 commit 尽量 `maybe_tidy` drain（旧 soft_cons） |
| duty8 / duty32 | 每 8 / 32 次 commit **最多 1 步** |
| hyst | `runs≥16` 才迈 1 步（开闸）；停靠 `tidy_runs` 压力门 |
| post | 写路径同 Soft；**写完**再最多 32 步 `maybe_tidy` |
| polish | 写完 `polish()` |

Assumption：动态 = 控制「何时付 consolidate 写」，不是改 `(a)(b)(c)` 合并公式。

## 2. 一次结果

### Soft sync 50k

| arm | cursors | range | insert | peak_x | steps |
|-----|--------:|------:|-------:|-------:|------:|
| soft | 12.38 | 28.3 | **7.0** | **1.44** | 0 |
| eager | 2.34 | 6.7 | 21.8 | 3.11 | 214 |
| **duty8** | **1.20** | 6.3 | **8.1** | 3.21 | 19 |
| duty32 | 3.79 | 15.2 | 7.0 | **1.78** | 4 |
| hyst | 12.38 | 23.0 | 6.5 | 1.44 | 0（终态 runs=10&lt;16） |
| **post** | 3.75 | 10.6 | **7.5** | **1.47** | 2 |
| polish | 1.25 | **4.1** | 7.6 | 3.07 | 0 |

### Soft sync 200k

| arm | cursors | range | insert | peak_x | steps |
|-----|--------:|------:|-------:|-------:|------:|
| soft | **6.48** | **18.4** | **7.0** | **1.44** | 0 |
| eager | 1.16 | 5.8 | **53.6** | **6.95** | 1096 |
| duty8 | **15.5↑** | 31.5↑ | 7.1 | 2.06 | 75 |
| duty32 | **31.6↑** | 49.6↑ | 6.8 | 2.22 | 18 |
| hyst | **15.9↑** | 34.3↑ | 9.8 | 1.76 | 39 |
| **post** | **1.30** | **5.1** | **6.2** | 3.66 | 6 |
| polish | 1.30 | 5.0 | 5.7 | **2.53** | 0 |

## 3. 判读

1. **写中限速（duty/hyst）在 200k 上反而伤 range**：半成品分层/多源相交 → cursors 比 Soft 基线更差。50k 上 duty8 看似好看，**不可外推**。  
2. **eager** 仍是 range 好、写/峰炸（与坑 147 一致）。  
3. **post（写后补做）**：写税贴 Soft，cursors/range 贴 polish 量级；peak_x 高于 Soft（整理产物占尾），但远好于 eager。  
4. **polish** 仍是读服务上界；post ≈「有界分步版写后整理」。

**leading**：动态整理若要产品化，优先 **写后 / 空闲补做**（post 或 polish），**不要**默认「写路径上 duty-cycle consolidate」。写中动态在大画像上可证伪为 range 有害。

## 4. 与升默认门禁的关系

| 候选 | 是否靠近 §4.2 门禁 |
|------|-------------------|
| Soft 默认 eager/duty | **否**（税或 range 回归） |
| Soft 默认 Post（写中记债 + 显式 `soft_cons_repay`） | **已落地**（坑 151）；非写中 auto-step |
| 文档：应用写完 `maybe_tidy` 循环 / `polish` | **已满足可选项叙事** |

## 5. 非目标 / 下一步

- 引擎 **Idle** 已落地（坑 149 · `DISCUSS_RANGE_IDLE.md`）：连续谷才整理；仍默认 Off。  
- post/polish 仍是读服务上界。  
- Soft A1 峰门禁：Idle 升默认前必跑。
## 6. 复现

```bash
bash scripts/ablate_range_dyn.sh
```
