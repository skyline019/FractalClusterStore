# FractalClusterStore v0.2.1

Maintenance release on top of **v0.2.0** (Soft default Post). Same public API and Soft defaults; engineering header split, full eval refresh, and documented read-path floor.

**Date:** 2026-11-05 · **Author:** skyline019

## Highlights

- **Header split (pit 169)** — Mechanical extract of `include/fcs.h` into `fcs_codec.h` / `fcs_dir.h` / `fcs_sync.h` / `fcs_store.h` plus a thin aggregator. Public `#include "fcs.h"` unchanged. Pre-split tree retained under `backup/eng-debt-split/` (do not delete). Gates: `make bins` · `make ci-smoke` · `make test` green.
- **Full eval (2026-11-05)** — `backup/eval-full/2026-11-05/EVAL_FULL.md`: `make test` + `soak-smoke` + `make asan` (TSan via `setarch -R`) PASS; Soft A1 peak_x **~1.84**; peers **×5 median** published.
- **Read / range floor** — Oneseg / open-fast / page-view / tip KEEP (pits 158–166); fence+seek load **Negate** (167). Short-window range vs LMDB ≈ **1.2×** median after Post repay—non-mmap floor, not a Soft default change.
- **Docs** — English GitHub README; EVAL17 / CONSTRAINTS / DESIGN3 / LaTeX handbook updated for the split and eval-full KPI.

## Upgrade notes

| From v0.2.0 | Change |
|:---|:---|
| Includes | Still `#include "fcs.h"` only. Implementation lives in `fcs_*.h` (pit 169). |
| Soft / Post / P2 | **Unchanged** product defaults (`configure_deploy`, Post, hole protect, P2 Soft-on). |
| Peers narrative | Prefer eval-full **×5 median** for sync/async ratios; peers-post remains the archived Soft Post table. |
| Disk format | Unchanged. |

No API removals. Keys remain `u64`; single-process.

## Build & test

```sh
make            # full regression
make ci         # local Strict CI without sanitizers
make asan       # ASan + UBSan + TSan
make ci-smoke   # smoke subset
```

Entry: [`README.md`](../../README.md) · Eval: [`EVAL17.md`](EVAL17.md) · Full pack: [`EVAL_FULL.md`](../../backup/eval-full/2026-11-05/EVAL_FULL.md) · Split: [`ENG_DEBT_SPLIT.md`](../30-discuss/ENG_DEBT_SPLIT.md) · Prior: [v0.2.0](RELEASE_NOTES_v0.2.0.md)

## Known gaps

Single-process; fixed `u64` keys; no hour-scale soak / true power-loss contract; `get` CRC still opt-in (`set_verify_reads`); short-window range vs LMDB remains a non-mmap floor (pits 145 / 167). Header split is **done** (pit 169).
