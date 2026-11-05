# FractalClusterStore v0.2.0

**Soft 默认 Post** 产品里程碑（在 v0.1.0 R17/R18 Soft 之上）。HEAD 对齐 Strict CI 全绿（含 ASan/TSan）。

## Highlights

- **SoftCons::Post（坑 150/151）**：`configure_deploy` / Soft 默认写中只记债；对外读前 `soft_cons_repay()` 或 `polish()`。消融：`set_soft_cons(Off)`
- **P2 Soft-on（坑 144）**：洞饿 `starve_fit` + `starve_promote` 进 Soft 默认
- **Peers 基线**：`backup/peers-post/PEERS_POST.md`（vs SQLite / LMDB；async 写 ~0.05–0.06×；sync+repay range 可反超 SQL）
- **Strict CI**：main 不 `cancel-in-progress`（坑 152）；多写者 `maint_loop` `ml_mu_` 串行（坑 153）；`tsan_g17` 时序与失败日志（坑 154）

## Upgrade notes

| 从 v0.1.0 Soft | 行为变化 |
|:---|:---|
| `configure_deploy` / Soft | 默认 `SoftCons::Post`（不再是「无 Post 的写换读 live」叙事） |
| 读路径 | 需要稳定 range/点查时：写后调用 `soft_cons_repay()` 或 `polish()` |
| P2 | Soft 默认开洞饿刀；消融仍可 `set_part_starve_fit(false)` 等 |

API 未删；键仍固定 u64、单进程。

## Build & test

```sh
make          # 全量测试
make ci       # 本地对齐严格 CI（不含 sanitizers）
make asan     # ASan + UBSan + TSan
make peers-post   # Soft Post peers（若已装对照库）
```

入口：[`README.md`](../../README.md) · 评估：[`EVAL17.md`](EVAL17.md) · 契约：[`CONSTRAINTS.md`](../../CONSTRAINTS.md) · 上一版：[v0.1.0](RELEASE_NOTES_v0.1.0.md)

## Known gaps

单进程、键固定 u64；无小时 soak / 真拔电合同；相对 LMDB range ~2.5×（单 run 地板，坑 145）；**发布时** `fcs.h` 拆头未做（其后 2026-11-05 坑 **169** 已完全拆头，见 `ENG_DEBT_SPLIT.md`）；`get` 默认不 CRC。
