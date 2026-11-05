# Agent 入口 — FractalClusterStore（`embdb/x`）

产品路径：**FCS / XDB v3**；公开 `#include "fcs.h"`（聚合），实现 `include/fcs_{codec,dir,sync,store}.h`（坑 169）。

## 必读

1. [`CONSTRAINTS.md`](CONSTRAINTS.md) — 质量契约（不可静默削弱）
2. [`docs/00-entry/EVAL17.md`](docs/00-entry/EVAL17.md) — 优势/缺口/下一轮优先级；公开标签见 [`RELEASE_NOTES_v0.2.1.md`](docs/00-entry/RELEASE_NOTES_v0.2.1.md)
3. [`docs/10-design/PIT_LOG.md`](docs/10-design/PIT_LOG.md) — 坑占号与闭环
4. 工作区规则：`../../.cursor/rules/`（`fcs-*.mdc`）
5. 工程技能：`../../.cursor/skills/fcs-*`

## 默认行为

- 改引擎 → `make ci-smoke`；触及提交/合并/TRIM/锁 → 再 `make test`（能跑则 `make asan`）
- 新坑 → **先** PIT_LOG 占号，再改代码，再补回归测
- **收工** → 有坑/近误判/环境注意就更新 `PIT_LOG.md`（同轮，勿只留聊天）
- **根因** → 复现 + 排除竞品假说 + 独立复核；未验证前写 leading，不宣称「就是根源」
- Soft / `configure_deploy` 是产品默认：**护洞 + P2 + SoftCons::Post**；勿退回 mode 0 / 关护洞 / 关 Post 当默认
- 读 SLA：写后 `soft_cons_repay()` 或 `polish()`；消融用 `set_soft_cons(Off)`
- Eager `soft_consolidate` / 写中 duty tidy：**勿**升 Soft 默认（坑 147）
- 双 MemTable / flush 出锁（坑 156 · `DISCUSS_CONCUR_IMM`）：**可选、Soft 默认关**；S5 判决 **Negate-keep**（`VERDICT_DUAL_MEM`）；勿当空间 KPI；勿进 `configure_deploy` 默认
- 拆头已完成（坑 169）；再切头 / P2c：先讨论稿，后薄切片、行为零 diff；热路径 peers：`make peers-post`

## 非目标

SQL、多进程共享同一打开库、把「未 repay 的 Soft live range≈LMDB」或「单 run range 追平 LMDB」当必须 KPI。
