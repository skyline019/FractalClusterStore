# FractalClusterStore v0.2.2

**Converge / ops** release on top of **v0.2.1**. Same public API and Soft product defaults; freezes the Soft surface, records hour soak, ships Soft recipe + example.

**Date:** 2026-11-05 · **Author:** skyline019 · **HEAD:** tag `v0.2.2`

## Highlights

- **Converge/ops ACTIVE (pit 170)** — Charter [`CONVERGE_OPS.md`](../30-discuss/CONVERGE_OPS.md): freeze Soft entry (`configure_deploy` + repay/`polish`); block new hot-path knives and promoting Negate-keep into Soft defaults until exit.
- **Soft recipe** — [`SOFT_RECIPE.md`](SOFT_RECIPE.md) · `make soft-example` (`examples/soft_u64_log.cpp`).
- **Dead-branch cleanup (pit 171)** — Remove unused `limit` truncation inside `select_oneseg_` (short-window oneseg stays Negate; entry still gated `limit==0`). dual_mem / space_accel / P1* **kept** as ablation APIs (Soft off).
- **Hour soak (pit 172)** — `FCS_SOAK_SEC=3600` Soft + `set_verify_reads(true)`: ops=745228 · commits=106694 · verifies=372 · **PASS** (`backup/converge/soak_3600_20261105.log`). **Not** a true power-loss contract.
- **Docs / handbook** — EVAL17, README, LaTeX (38 pp) updated for converge 170–172.

## Upgrade notes

| From v0.2.1 | Change |
|:---|:---|
| Soft / Post / P2 / tip / read knives | **Unchanged** product defaults |
| Includes | Still `#include "fcs.h"` only |
| oneseg select | Behavior for `limit==0` unchanged; dead `limit` branch inside oneseg removed |
| Disk format | Unchanged |
| Ops posture | Prefer Soft recipe; do not retune Negate-keep knobs as “product” |

## Build & test

```sh
make ci-smoke
make soft-example
make            # full regression
# optional: FCS_SOAK_SEC=3600 make soak
```

Entry: [`README.md`](../../README.md) · Eval: [`EVAL17.md`](EVAL17.md) · Converge: [`CONVERGE_OPS.md`](../30-discuss/CONVERGE_OPS.md) · Prior: [v0.2.1](RELEASE_NOTES_v0.2.1.md)

## Known gaps

Single-process; fixed `u64` keys; no true power-loss contract; `get` CRC opt-in; short-window range vs LMDB non-mmap floor (145 / 167). Header split done (169); hour soak recorded (172).
