# 工程债一期评估（EVAL_ENG_DEBT_P1）

对象：EVAL17 §8 第 3 项「工程：soak / set_verify_reads / 拆头 / 严格测试 / 坑纪律」在本工作区的落地。  
基线：`CONSTRAINTS.md` · `PIT_LOG.md` · Soft 产品默认不变。  
日期：2026-11-03。

---

## 0. 一页结论

| 维度 | 一期目标 | 结果 | 证据 |
|------|----------|------|------|
| 读路径 CRC | 可选校验 + 回归 | **达标** | 坑 120 FIXED · `c2` · `set_verify_reads` |
| Soak 短门 | 门禁化短时跑 | **达标** | 坑 121 FIXED · `soak-smoke` 30s PASS |
| 严格测试 | ci-smoke / test / asan(+tsan) | **达标*** | 见 §2；TSan 需 `TSAN_WRAP`（坑 122） |
| 坑纪律 | 活表 + 收工更新 + 根因验证 | **达标** | rules/skills · 坑 120 近误判 / 122 NOTE |
| 拆头 | 机械切片启动 | **未做** | 仅 `ENG_DEBT_SPLIT.md` 计划 |
| 真拔电 / 长 soak / CI 上 c2 | EVAL 原文「工程」子集 | **未纳入一期完成定义** | 仍靠 f3/g6/h3；GitHub CI smoke 未强制改 |

\*「达标」= 本机 WSL 已实践绿；GitHub Actions 是否已含 `c2`/`TSAN_WRAP` 以远端工作流为准（本地 Makefile 已改）。

**一句话**：工程债一期把「可测的正确性护栏 + 流程纪律」做完；**拆头与长时/真掉电矩阵留给二期**，不宣称耐久合同已闭合。

---

## 1. 对照 EVAL17 §8.3

| 原文项 | 一期 | 说明 |
|--------|------|------|
| `set_verify_reads` | 做完 | 默认关；开启清缓存；活页 CRC |
| soak | 短门做完 | 长时 `make soak` 能力有，未跑小时级作为门禁 |
| 拆头 | 未做 | 零 diff 计划已写 |
| 真掉电 | 未扩 | 既有 f3/g6/h3；非一期新增 |
| 本地 git | 未评 | 工作区/远程策略不在本轮 |

---

## 2. 严格门禁实测（本机）

| 门 | 结果 | 备注 |
|----|------|------|
| `make ci-smoke` | PASS | 含 `c2` |
| `make test` | PASS | ~11 min；含 `c2`/`b1` |
| `make soak-smoke` | PASS | 30s |
| ASan/UBSan 段 | PASS | 含 `c2`；`g13` ~243s |
| TSan 段 | PASS（经 wrap） | 坑 122：裸跑 FATAL；`setarch -R` 后全绿 |

---

## 3. 质量与过程得失

**得**

- CRC 与 soak 有测、有坑号、进 Makefile 门。
- Soft 默认未回退；否定旋钮未复活。
- 坑 120 测试近误判已写入日志（退役页 / `used` 外翻位）。
- 根因纪律落地：122 排除「产品竞态」假说。

**失 / 债**

- `fcs.h` 仍 ~8k 行；拆头一期零进度。
- 小时 soak、真拔电矩阵、远端 CI 与本地对齐未在一期验收。
- `make asan` 在 `/mnt/e` 上墙钟偏长（串行编+跑 + 9p）；属效率债。

---

## 4. 二期建议（按性价比）

1. **拆头切片 A**（`fcs_codec.h`）— 行为零 diff，按 `ENG_DEBT_SPLIT` 检查单。  
2. **CI**：远端 smoke 确认含 `c2`；asan job 确认 TSan wrap 或等价 sysctl。  
3. **夜间**：`FCS_SOAK_SEC=3600 make soak` 一次，结果贴 PIT_LOG NOTE（仍不替代掉电测）。  
4. 再回头 EVAL17 产品轨：P2c 或双 memtable（与工程债正交）。

---

## 5. 判据签字栏

| 问题 | 答案 |
|------|------|
| 一期工程债是否可宣布「主目标完成」？ | **是**（CRC + 短 soak + 严格测试 + 坑纪律） |
| 是否可宣布「EVAL17 工程项全部关闭」？ | **否**（拆头 / 真掉电 / 长 soak 未关） |
| Soft peers / 合并峰是否因本轮变化？ | **未测** → 不改 EVAL 数字（`KPI_UNVERIFIED` 不适用：无热路径 KPI 宣称） |
