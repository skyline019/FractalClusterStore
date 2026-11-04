# Agent 入口 — FractalClusterStore（`embdb/x`）

产品路径：**FCS / XDB v3**，主实现 `include/fcs.h`。

## 必读

1. [`CONSTRAINTS.md`](CONSTRAINTS.md) — 质量契约（不可静默削弱）
2. [`docs/00-entry/EVAL17.md`](docs/00-entry/EVAL17.md) — 优势/缺口/下一轮优先级
3. [`docs/10-design/PIT_LOG.md`](docs/10-design/PIT_LOG.md) — 坑占号与闭环
4. 工作区规则：`../../.cursor/rules/`（`fcs-*.mdc`）
5. 工程技能：`../../.cursor/skills/fcs-*`

## 默认行为

- 改引擎 → `make ci-smoke`；触及提交/合并/TRIM/锁 → 再 `make test`（能跑则 `make asan`）
- 新坑 → **先** PIT_LOG 占号，再改代码，再补回归测
- **收工** → 有坑/近误判/环境注意就更新 `PIT_LOG.md`（同轮，勿只留聊天）
- **根因** → 复现 + 排除竞品假说 + 独立复核；未验证前写 leading，不宣称「就是根源」
- Soft 是产品默认；不要把 mode 0 / 关护洞设回默认
- 大改拆头 / P2c / 双 memtable：先讨论稿，后薄切片

## 非目标

SQL、多进程共享同一打开库、把「未整理 range≈LMDB」当必须 KPI。
