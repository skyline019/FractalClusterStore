# Agent 入口 — FractalClusterStore（`embdb/x`）

产品路径：**FCS / XDB v3**；公开 `#include "fcs.h"`（聚合），实现 `include/fcs_{codec,dir,sync,store}.h`（坑 169）。

## 工作模式（现行）

**收敛运维 ACTIVE（坑 170）** — 读 [`docs/30-discuss/CONVERGE_OPS.md`](docs/30-discuss/CONVERGE_OPS.md) 与 [`docs/00-entry/SOFT_RECIPE.md`](docs/00-entry/SOFT_RECIPE.md)。

- 默认只做：正确性修、文档/示例、工程债运维、有证据删死码
- **不要**开新热路径优化弧 / 升 Negate-keep 默认 / 追 LMDB range 地板
- 退出收敛须用户明示或 CONVERGE_OPS → EXITED + PIT NOTE

## 必读

1. [`CONSTRAINTS.md`](CONSTRAINTS.md) — 质量契约（不可静默削弱）
2. [`docs/00-entry/EVAL17.md`](docs/00-entry/EVAL17.md) — 优势/缺口；标签 [`RELEASE_NOTES_v0.2.2.md`](docs/00-entry/RELEASE_NOTES_v0.2.2.md)
3. [`docs/10-design/PIT_LOG.md`](docs/10-design/PIT_LOG.md) — 坑占号与闭环
4. 工作区规则：`../../.cursor/rules/`（`fcs-*.mdc`）
5. 工程技能：`../../.cursor/skills/fcs-*`（含 `fcs-converge`）

## 默认行为

- 改引擎 → `make ci-smoke`；触及提交/合并/TRIM/锁 → 再 `make test`（能跑则 `make asan`）
- 新坑 → **先** PIT_LOG 占号，再改代码，再补回归测
- **收工** → 有坑/近误判/环境注意就更新 `PIT_LOG.md`（同轮，勿只留聊天）
- **根因** → 复现 + 排除竞品假说 + 独立复核；未验证前写 leading，不宣称「就是根源」
- Soft / `configure_deploy` 是产品默认：**护洞 + P2 + SoftCons::Post**；勿退回 mode 0 / 关护洞 / 关 Post 当默认
- 读 SLA：写后 `soft_cons_repay()` 或 `polish()`；消融用 `set_soft_cons(Off)`
- Eager `soft_consolidate` / 写中 duty tidy：**勿**升 Soft 默认（坑 147）
- 双 MemTable / flush 出锁（坑 156）：**可选、Soft 默认关（Negate-keep）**；勿进 `configure_deploy`
- 拆头已完成（坑 169）；再切头须行为零 diff；热路径 peers 仅在**退出收敛后**作为优化弧 KPI

## 非目标

SQL、多进程共享同一打开库、把「未 repay 的 Soft live range≈LMDB」或「单 run range 追平 LMDB」当必须 KPI。
