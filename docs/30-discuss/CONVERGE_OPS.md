# Converge / Ops mode（收敛运维）

状态：**ACTIVE** · 2026-11-05 · 坑 **170** / 删码 **171** / 小时 soak **172**  
入口：`CONSTRAINTS.md` §收敛 · `AGENTS.md` · skill `fcs-converge`

## 为何进入

优化弧（157–167）与工程债拆头（169）已收口；peers ×5 / Soft A1 / Strict CI 绿。
继续堆读刀或 Negate-keep 实验臂 = 密度下降。本模式目标是 **冻结产品面、可运维、可讲述**，不是再涨 KPI。

## 允许（运维面）

| 类 | 例子 |
|----|------|
| 正确性 / 静默失败 | 新坑 → 先占号 → 最小修 → 回归 |
| 文档 / 示例 / README | Soft 食谱、接入故事、缺口诚实表述 |
| 工程债运维 | 长 soak 记录、CI 对齐、拆头后再切须零 diff |
| 观测 | 探针只读；不默认改热路径 |
| 删死代码 | 仅当：有 PIT NOTE + 消融已 Negate + `make test`/`ci-smoke` 绿 + 不进 Soft 默认 |

## 禁止（未退出收敛前）

- 新热路径「优化」旋钮或第三套整理哲学
- 把 Negate-keep 臂升 Soft / `configure_deploy` 默认（dual_mem、space_accel、coalesce B、P1*、短窗 oneseg、fence-seek load…）
- 默改读模型追 LMDB mmap 地板（坑 167）
- 扩 SQL / 多进程 / 非 u64 键（须显式产品决议 + 退出收敛）
- 无 `make test` / `ci-smoke` 的「先合再说」

## 冻结的产品 Soft 面（唯一推荐入口）

```cpp
s.configure_deploy(/*async=*/true);  // Soft + hole + P2 + Post + tip/read knives
// write …
s.commit();
s.soft_cons_repay();                 // or polish() before external reads
```

成员级默认见 `backup/work-dist/OPT_COMPLETE.md`。消融仅用于 A/B，不改产品叙事。

## 删除候选审计（坑 171 · 2026-11-05）

| 项 | 消融证据 | 判决 | 动作 |
|----|----------|------|------|
| **短窗 oneseg** | 166 Negate ~1.0×；入口已闸 `limit==0` | **删死枝** | 去掉 `select_oneseg_` 的 `limit` 参数与截断枝（171）；短窗永不进 oneseg |
| **tip.osz（经典）** | 163：经典 bloom-only；曾用 tip.osz ⇒ g16 漂 | **已无死码** | 经典 `account_mem_run_` **不传 prev**；注释钉死。`DualTip.osz` 仅 dual_mem 用 |
| **dual_mem 整臂** | 156 Negate-keep；`g18` t_dual_mem | **保留旋钮** | Soft 默认关；禁止进 `configure_deploy`。删整臂 = 大 diff + 失消融门，收敛期不做 |
| **space_accel / P1\*** | Soft 勿升；`c3`/`c4`/`c5` 消融门 | **保留 API** | 默认关；非死码。收敛期不删 |
| fence+seek load | 167 Negate 1.29× 已回滚 | **已无** | 无残留旗标/枝 |

**原则**：Negate-keep ≠ 立刻删 API；只有「无调用方 + Soft 永不需要」才删实现。本轮只删已确认死枝。

## 退出收敛

须书面改本文件状态为 **EXITED** + PIT NOTE，并满足其一：

1. 用户显式「退出收敛 / 开优化弧」；或  
2. 正确性 OPEN 坑迫使热路径改动（修完可回收敛）。

## 检查单

- [x] ACTIVE + 坑 170
- [x] CONSTRAINTS / AGENTS / fcs-core 冻结句
- [x] Soft 食谱 + `examples/soft_u64_log.cpp`
- [x] 删码表逐项审计（坑 171）；短窗 oneseg limit 死枝已删
- [x] 小时 soak NOTE（坑 172 · `backup/converge/soak_3600_20261105.log` PASS）
- [ ] （可选）若弃 dual_mem 消融门再开删臂 PR；夜间周期性重跑 soak
