# FCS 完整全量评估 · 2026-11-05

对象：Soft Post 产品主树（评测时 `include/fcs.h` **9405** 行整头）· HEAD `5eb2a1f`  
复现：`STAMP=2026-11-05 bash scripts/eval_full.sh` → 本目录  
墙钟：~10:36–11:48（约 **72 min**，其中 **asan ≈ 47 min**）

> **补记（同日稍后 · 坑 169）**：完全拆头已落地（`fcs_codec/dir/sync/store.h` + 薄 `fcs.h`）；本卷宗数字仍为拆前主树实测。拆后门禁：`backup/eng-debt-split/make_test.log`。

## 0. 一页结论

| 门禁 | 结果 |
|------|------|
| `make bins` / `make test` / `soak-smoke` | **PASS** |
| `make asan`（ASan+UBSan+TSan，`setarch -R`） | **PASS**（未卡死，见 §1） |
| peers ×5 中位 | **PASS**（§2） |
| Soft A1 空间峰 | **PASS** peak_x **1.836**（§3） |

**一句话**：正确性/消毒全绿；产品 Soft Post 对 SQL 读碾压、写≈持平；对 LMDB 写/空间强、get≈平、短窗 range ~**1.2×**；空间峰 ~1.84× 与历史 Soft A1 同量级。

## 1. ASan 是否卡死？

**否。** 进程一直在推进：

| 时刻（约） | 状态 |
|------------|------|
| 10:59 | 进入 `make asan` |
| ~11:30 | 主循环已过 g13（ASan **234 s**）…h3 |
| ~11:33 | 编译 `asan_s4` → `asan_c1`（仍活） |
| 11:46 | ASan+TSan 全段 **PASS** |

假死观感来自 Makefile：`build/asan_$t | tail -1` 在管道里缓冲，**长时间看不到新行**。实际 CPU 在编/跑。后续用 `scripts/asan_progress.sh` 可逐测刷进度。

TSan 走 `setarch $(uname -m) -R`（坑 122 WSL 映射陷阱）。

## 2. 性能 / 空间（peers ×5 · 产品 tip=on dual=off）

比值 = **各轮 FCS/对手 的中位**（推荐口径）。空间绝对值：sync **2.50 MB** · async **2.15 MB**。

| | sync/SQL | sync/LMDB | async/SQL | async/LMDB |
|--|--:|--:|--:|--:|
| insert | **0.96×** [0.88–1.27] | **0.95×** [0.83–1.13] | **0.10×** | **0.07×** |
| update | **0.63×** | **0.59×** | **0.07×** | **0.07×** |
| get | **0.24×** | **0.97×** | **0.20×** | **1.06×** |
| range | **0.26×** | **1.18×** [1.05–1.31] | **0.22×** | **1.17×** [1.16–1.97] |
| space | 1.12× | **0.47×** | 0.96× | **0.41×** |

消融（FCS 绝对值中位）：tip on/off insert **0.88×** → **KEEP tip**；dual on/off insert **1.13×** → **Negate-keep dual off**。

源：`PEERS_FULL_MED.md` · `peers/med_*_r*.log` · `summarize_ratios.py`

## 3. Soft A1 空间峰

| 指标 | 值 |
|------|-----|
| peak_x | **1.836**（peak_tail=3401） |
| end_x | 1.479 |
| AT_PEAK | c569 · HOLE_STARVE 主导 · dead/live=0.021 → **勿开 P2c** |
| maint | starve_fits=4 · hole_protect_skips=19 |

与历史 Soft A1（~1.74–1.86）同带。源：`soft_a1.log`

## 4. 风险 / 地板（相对 EVAL17 滚动）

| # | 项 | 本评 |
|---|-----|------|
| 1 | 短窗 range vs LMDB ~1.2× | 坑 167：emit≈1 + 非 mmap；勿堆 Negate load 刀 |
| 2 | sync durable 写 | phase2 I/O 地板；async 仍碾压 |
| 3 | Soft A1 峰 ~1.8× | 养洞/PEND；P2c 门未开 |
| 4 | 无小时 soak / 真拔电 / 多进程 | 覆盖缺口不变 |
| 5 | 单头 9.4k 行 | **评测时未拆**；同日稍后坑 **169** 已关（见补记） |

## 5. 产物清单

```
backup/eval-full/2026-11-05/
  PASS.txt FAIL.txt run.log
  make_test.log soak_smoke.log asan.log soft_a1.log
  PEERS_FULL_MED.md peers/ summarize_ratios.py
  EVAL_FULL.md          # 本文件
```
