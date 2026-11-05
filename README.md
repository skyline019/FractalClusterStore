# FractalClusterStore

<p align="center">
  <strong>Embedded ordered key–value store</strong><br>
  <sub>Product name <b>FCS</b> · Implementation <b>XDB v3</b> · Public API <code>include/fcs.h</code></sub>
</p>

<p align="center">
  <a href="https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml"><img src="https://github.com/skyline019/FractalClusterStore/actions/workflows/ci.yml/badge.svg" alt="Strict CI"></a>
  <a href="https://github.com/skyline019/FractalClusterStore/releases/tag/v0.2.2"><img src="https://img.shields.io/github/v/release/skyline019/FractalClusterStore" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17">
  <img src="https://img.shields.io/badge/keys-u64-lightgrey.svg" alt="u64 keys">
</p>

FractalClusterStore (**FCS**) is a single-process, embeddable ordered KV engine for devices and co-located services. Data lives in append-only **runs**; the directory is a fractal **cell → segment → run → page** view—balance without a B+ tree.

| | |
|:---|:---|
| **Latest release** | [v0.2.2](https://github.com/skyline019/FractalClusterStore/releases/tag/v0.2.2) · [notes](docs/00-entry/RELEASE_NOTES_v0.2.2.md) ([v0.2.1](docs/00-entry/RELEASE_NOTES_v0.2.1.md) · [v0.2.0](docs/00-entry/RELEASE_NOTES_v0.2.0.md)) |
| **Working mode** | **Converge/ops ACTIVE** (pits 170–172) — Soft surface frozen; see [`CONVERGE_OPS.md`](docs/30-discuss/CONVERGE_OPS.md) |
| **CI** | Strict CI: Compile (`-Werror`) → Smoke → Full test → Sanitizers → **Strict CI OK** |
| **License** | [MIT](LICENSE) |

---

## Why FCS

- **Write amp ~2.7×** on comparable loads (vs historical B+ paths at 22–56×)
- **Soft deploy defaults**: async I/O, hole protect, P2 starve knives, **`SoftCons::Post`** (debt on write; repay before serving reads)
- **Commit fsync off the exclusive lock** — readers are less likely to stall on durable commit
- **Peers**: strong vs SQLite on write/space/point get; short-window range vs LMDB is a non-mmap floor (~1.2× median), not a default-knob chase

> **Not a goal:** SQL, multi-process shared open of one DB, or “live Soft range ≈ LMDB without repay.”

---

## Features

| Area | Capability |
|:---|:---|
| **Data model** | Fixed-width `u64` keys; point get; range / prefix scan; relocatable cursors; snapshot views; secondary indexes (same commit as primary) |
| **Durability** | Dual-slot superblock + manifest; reuse gated on synced SB; `durable_vers()` |
| **Maintenance** | Leveled merge, partitioned TRIM, hole protect, P2; Soft / Hard / Periodic tidy |
| **Integrity** | `verify` / `verify_ownership` / `scrub`; optional `set_verify_reads` (data-page CRC on cache fill; **off** by default) |
| **Capacity** | 32-bit page id × 4 KiB ≈ 16 TiB addressable; value size ≤ 65 534 B |

---

## Quick start

**Requires:** C++17, `g++`, `make` (validated on Linux / WSL).

```sh
make ci-smoke   # CI-aligned smoke
make            # full regression
make asan       # ASan / UBSan / TSan (WSL TSan: setarch -R)
```

```cpp
#include "fcs.h"
using namespace fcs;

RamDev dev(4096);
Store s;
u8 arena[512 * PAGE];
if (s.open(&dev, arena, 512) != RC::OK) return 1;

s.configure_deploy();          // Soft + hole protect + P2 + SoftCons::Post
s.put(42, "hello", 5);
if (s.commit() != RC::OK) return 1;
s.soft_cons_repay();           // repay before read SLA (or polish())

u8 buf[64];
u16 n = 0;
s.get(42, buf, sizeof buf, &n);
```

Full recipe: [`SOFT_RECIPE.md`](docs/00-entry/SOFT_RECIPE.md) · `make soft-example` · converge mode: [`CONVERGE_OPS.md`](docs/30-discuss/CONVERGE_OPS.md) (pit 170).

| Item | Path |
|:---|:---|
| Public include | `include/fcs.h` (thin aggregator) |
| Implementation | `fcs_codec.h` · `fcs_dir.h` · `fcs_sync.h` · `fcs_store.h` |
| Device / layout | `blockdev.h` · `layout.h` · `common.h` |
| File device | `FileDev` |

---

## Architecture

```text
App
 └── Store                 include/fcs.h → fcs_store.h
      ├── MemTable         fcs_dir.h · flush → L0 run
      ├── cell / seg / run Bloom · fences · key-range summary
      ├── Manifest + SB    crash recovery (dual slot)
      ├── Maintenance      leveled merge · partitioned TRIM · Soft/Hard/Periodic
      └── BlockDev         RamDev · FileDev · custom
```

| Topic | Notes |
|:---|:---|
| **Balance** | Units are views over immutable runs—split/merge without rewriting page payloads |
| **Commit** | Phase-2 fsync can run outside the exclusive lock; visibility still precedes durability |
| **Soft** | Write path records tidy debt; call `soft_cons_repay()` / `polish()` before external reads |

Design: [`DESIGN3.md`](docs/10-design/DESIGN3.md) · Evaluation: [`EVAL17.md`](docs/00-entry/EVAL17.md)

---

## Benchmarks (summary)

Same-batch peers vs **SQLite** / **LMDB** (50 000 keys, 32 B values, commit every 500). Product Soft = Post + repay. Prefer **×5 median** where noted.

### Soft async + cons + shrink · Post (archived peers-post)

| Metric | FCS | SQLite | LMDB | ÷SQL | ÷LMDB |
|:---|---:|---:|---:|---:|---:|
| Insert µs/op | 0.32 | 5.17 | 6.04 | **0.06×** | **0.05×** |
| Update | 0.66 | 10.1 | 11.7 | **0.06×** | **0.05×** |
| Get | 0.40 | 1.69 | 0.31 | 0.24× | ~1.0× |
| Range (100 rows) | 1.00 | 1.73 | 0.32 | 0.57× | ~2–3× |
| Space MB | **2.15** | 2.24 | 5.29 | 0.96× | **0.41×** |

### Soft product · ×5 median (2026-11-05 eval-full)

| | sync/SQL | sync/LMDB | async/SQL | async/LMDB |
|:---|---:|---:|---:|---:|
| Insert | 0.96× | 0.95× | **0.10×** | **0.07×** |
| Get | **0.24×** | 0.97× | **0.20×** | 1.06× |
| Range | **0.26×** | **1.18×** | **0.22×** | **1.17×** |
| Space | 1.12× | **0.47×** | 0.96× | **0.41×** |

Sources: [`PEERS_POST.md`](backup/peers-post/PEERS_POST.md) · [`EVAL_FULL.md`](backup/eval-full/2026-11-05/EVAL_FULL.md) · [`EVAL17.md`](docs/00-entry/EVAL17.md)

| Generation | Write amplification (approx.) |
|:---|---:|
| v1 | 56× |
| v2 | 22× |
| **v3 (FCS)** | **~2.7×** |

Soft A1 space peak ≈ **1.84×** (hole-starve / PEND phase; P2c stays off by default).

---

## Limits

- Single process; keys are `u64` only
- No multi-process file-lock protocol for one open store
- Hour-scale soak / true power-loss matrix are **not** claimed as closed contracts (`g6` / `f3` / `h3` remain crash gates)
- `get` does not CRC data pages unless `set_verify_reads(true)`
- Short-window range vs LMDB remains a non-mmap floor after oneseg/Post repay—see pits 145 / 167

---

## Documentation

| Doc | Contents |
|:---|:---|
| [DETAIL](docs/00-entry/DETAIL.md) | API excerpts, benches, test matrix |
| [EVAL17](docs/00-entry/EVAL17.md) | Correctness / perf / space / risk (rolling) |
| [DESIGN3](docs/10-design/DESIGN3.md) | Design + historical pit narrative |
| [PIT_LOG](docs/10-design/PIT_LOG.md) | Living pit register |
| [CONSTRAINTS](CONSTRAINTS.md) | Quality bar (do not silently weaken) |
| [Docs index](docs/README.md) | Rounds, discuss notes, LaTeX |
| [LaTeX handbook](docs/latex/README.md) | `cd docs/latex && make` → `build/main.pdf` |
| [fcs-anim.html](fcs-anim.html) · [fcs-pits.html](fcs-pits.html) | Offline walkthroughs |

---

## Layout

```text
include/      Public + implementation headers (FCS v3)
tests/        Regression suite (make test)
bench/        Peers and probes
docs/         Eval · design · rounds · discuss · latex
web/          Explainer page sources
scripts/      Ablation / eval / split helpers
backup/       Peers, eval-full, eng-debt-split (pre-split fcs.h)
```

Legacy v1 (`../src/engine.cpp`) and v2 (`include/store.h`) remain for comparison. **Product path is v3 / FCS.**

---

## Contributing & license

Released under the [MIT License](LICENSE).

| | |
|:---|:---|
| Issues | https://github.com/skyline019/FractalClusterStore/issues |
| Releases | https://github.com/skyline019/FractalClusterStore/releases |
| Agent entry | [`AGENTS.md`](AGENTS.md) |
