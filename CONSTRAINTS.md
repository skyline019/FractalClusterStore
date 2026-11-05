# FCS / XDB v3 — 质量契约（Constraint Bar）

> 代理人与人类改代码前必读。削弱本文件中的数字或门禁 = 违约，须先改本文件并说明理由。

## 收敛运维（ACTIVE · 坑 170）

> 章程：`docs/30-discuss/CONVERGE_OPS.md` · 食谱：`docs/00-entry/SOFT_RECIPE.md`

- **产品默认面冻结**：唯一推荐入口 `configure_deploy` + 读前 `soft_cons_repay`/`polish`。禁止未退出收敛就升 Negate-keep 臂进 Soft 默认。
- **禁止新热路径刀**：无消融胜证据不得合入；坑 167 地板勿再堆 load/range 微调。
- **允许**：正确性修、文档/示例、工程债运维（长 soak 记录等）、经确认的死码删除。
- **退出**：须改 `CONVERGE_OPS.md` 状态为 EXITED + PIT NOTE，或用户显式开新优化弧。

## Floor（永远阻塞合并）

| 检查 | 命令 / 判据 | 失败时 |
|------|-------------|--------|
| 编译 `-Werror`（已知 legacy 除外，见 CI） | `make bins` / Strict CI Compile | **Block** |
| Smoke | `make ci-smoke`（f1, g16–g18, h2） | **Block** |
| 全量回归 | `make test` | **Block** |
| Sanitizer | `make asan`（ASan/UBSan/LSan + 约定 TSan 子集） | **Block** |
| TSan on WSL | 新内核若 `FATAL: unexpected memory mapping`：Makefile 用 `setarch $(uname -m) -R` 包 TSan（环境问题，非产品缺陷）；探针 `scripts/tsan_env_probe.sh` | 勿当引擎 bug 关测 |
| 产品默认路径 | Soft + `maint_mode≥1`；不得默默退回 mode 0 或关护洞当默认 | **Block** |

## 工程债轨门禁（本轮强制）

| 维度 | 阈值 | 说明 |
|------|------|------|
| 读路径 CRC | `set_verify_reads(true)` 有测（`c2` 进 `make test` / `ci-smoke`）；默认 **关**；开启时清页缓存；坏活页不得 `get` OK | 坑 120 |
| Soak | `make soak-smoke`（默认 30s）挂在本地 `make ci`；长时 `make soak`；**不得**标「已覆盖真拔电」 | 坑 121 · `soak1` |
| 拆头 | **已完成**（坑 169 · `fcs_codec/dir/sync/store.h` + 薄 `fcs.h`）；后续再切须 **行为零 diff**（同负载 `make test`；Soft peers 关键 KPI 不动） | `docs/30-discuss/ENG_DEBT_SPLIT.md` · `backup/eng-debt-split/` |
| 坑记录 | 新正确性/静默失败缺陷 → `docs/10-design/PIT_LOG.md` **先占号**再合码；DESIGN3 §12 / `fcs-pits` 同轮或下一轮跟进 | Skill: `fcs-pit-discipline` |

## 明确禁止（静默降标）

- 删测、跳测、`#if 0` 掉断言、「先合再补测」
- 把已否定旋钮改回默认（并洞 B、cliff/脏度默认、小预留 TRIM 关当 KPI）
- 扩 SQL / 多进程共享库而不单开产品线与规格
- 改 `CONSTRAINTS.md` 数字却不写理由与复现命令
- 堆无法消融证明的「优化」或并行第三套整理哲学；消融无效 → **删代码**，只留 PIT NOTE

## 空间峰 / Harness（已拍板 · T 臂消融）

- 主 KPI：空间峰与可写性；画像 **A1 layout · A2 小盘(256/512/1024) · A3 Soft peers**
- 「动态卸载」：Soft **护洞保留**；`space_accel` **Soft 默认关**（`backup/harness-ab-space/VERDICT.md`：三画像无峰增益）
- 旋钮 `set_space_accel_pct` 可实验；非双 memtable；P2c 未证必要前不开
- **P1/P1b** `set_protect_stream_small`：整洞跳过与 anti-shatter 均 Soft A1 抬峰（坑 132/133 · `VERDICT_P1b.md` peak 1.86→2.20）；**Soft 默认关且勿升**
- **P1c/d** `set_stream_best_fit_mode` / `skip_pct`（坑 134/135）：消融最优实验臂 **mode=2 + skip=180**（`VERDICT_P1d.md`：峰↓≈15%、insert 无税）；**Soft 默认仍关**（A2 未同向）
- **P2** `set_part_starve_fit` / `set_starve_promote`（坑 138–144）：洞饿紧余量+封口放行。税刀+锁外 β；`part_cap_need` 看 `starve_tight`。Soft A1 峰≈−13%。**坑 144：显式升入 Soft / `configure_deploy` 默认开**（门禁可议通过；消融仍可 set false）。`VERDICT_P2_SOFT_ON.md` · `make soft-a1-p2-gate` · `make peers-p2`
- 开发期 sanitizer：**轻量** `make asan-new`（含 c3/c4/c6）；全量 `make asan` 本轮不做
- Harness：`make harness-ab-space`


## 性能 / 空间护栏（改热路径时）

以 `docs/00-entry/EVAL17.md` Soft peers 为基线；热路径 PR 至少声明是否跑过：

- layout 200k 合并峰（勿无测宣称「峰下降」）
- Soft peers 或同等 `bench4` 对照

未测则 PR 标注 **KPI_UNVERIFIED**，不得写入 EVAL 正文数字。

## 定位边界（非缺陷）

- 键固定 `u64`；单进程；非 SQL
- 未整理 range vs LMDB 慢 = 「写换读」权衡，靠 Soft + `polish`，不是默默改读模型
- **路径 B** `set_soft_cons`（坑 146–151）：`Off|Eager|Idle|Post`。**坑 151：`configure_deploy` / Soft 默认 Post**（写中记债；读前 `soft_cons_repay` 或 `polish`）。写税刀 `min_gap=32`/`run_hi=16`。消融 `set_soft_cons(Off)`。门禁：`backup/soft-a1/VERDICT_POST_SOFT_ON.md` · `scripts/soft_post_on_verify.sh`
- **坑 157**：Post 记债默认 **不** 走 `tidy_snap→free_`（`set_soft_post_debt_snap` 消融）；range k1 快路径已 Negate 删除
- **坑 158**：`range_open_fast` 默认开（空 mem + 单段 prime）；消融 `set_range_open_fast(false)`。peers range vs LMDB 剩余差 leading=页 load/memcpy + 每 select 新 open（非 k-way）
- **坑 159 / 162 / 163**：Soft sync 墙钟大头 = phase2 I/O（勿默减屏障）。`flush_tip` **默认开**（163：仅 bloom 跳首写 settle；禁止 tip.osz 免 probe）。消融 `set_flush_tip(false)`。完整 peers：`backup/peers-full/PEERS_FULL.md`。工作分布：`backup/work-dist/WORK_DIST.md`
- **坑 160**：Soft `configure_deploy` 开 `range_page_view`（扫描钉 page 免拷）；消融 `set_range_page_view(false)`。**select/scan 回调内禁止 get/page**（会挪 pin）
- **坑 161**：Soft `configure_deploy` 关 `range_pf_eager`（翻页才预取；首填不预取）。消融 `set_range_pf_eager(true)`。短窗/1 行扫 KEEP；全表扫 ≈中性
- **坑 164**：Soft `configure_deploy` 开 `range_oneseg_sel`（空 mem + `limit==0` 单段直扫）。短窗勿开 oneseg 直扫。消融 `set_range_oneseg_sel(false)`
- **坑 165**：`Cur::reuse()`（禁止 `Cur{}` 清 PAGE）；单段 prime 跳 `run_may_have`；`get` 走 `get_at_locked_`
- **坑 166**：`Cursor::next` 空 mem+单 Cur 快路径（无旋钮）。短窗 oneseg 再消融仍 Negate。peers ×5 中位 sync range/LMDB ~**1.31×**（`PEERS_FULL_MED.md`）
- **坑 167**：短窗 emit≈1；`Cur::load` fences+seek **Negate**（已回滚）。range vs LMDB 剩余=随机首页/非 mmap 地板，勿堆无消融胜微调
- **并发错峰 A** 双 MemTable / `flush_mem` 出锁（坑 156 · `DISCUSS_CONCUR_IMM.md` · `VERDICT_DUAL_MEM`）：**可选、Soft 默认关（Negate-keep）**；KPI=锁内延迟/读者 max，**勿**当空间峰胜者；**禁止**进 `configure_deploy` 默认
- **坑 170**：收敛运维 ACTIVE — 冻结上表 Soft 面；食谱 `SOFT_RECIPE.md`；示例 `make soft-example`
- **坑 171**：收敛删死码 — 短窗 oneseg limit 枝已删；dual_mem / space_accel / P1* **保留消融 API**（勿当 Soft 默认）
- **坑 172**：收敛期 `FCS_SOAK_SEC=3600` Soft+CRC 读校验 **PASS**（`backup/converge/`）；仍非真拔电
