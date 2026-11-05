# FractalClusterStore v0.2.0

**Soft default Post** product milestone (on top of v0.1.0 R17/R18 Soft). HEAD at tag time aligned with Strict CI green (ASan/TSan).

> **Current release:** [v0.2.2](RELEASE_NOTES_v0.2.2.md) · prior [v0.2.1](RELEASE_NOTES_v0.2.1.md)

## Highlights

- **SoftCons::Post (pits 150/151)** — `configure_deploy` / Soft default: record tidy debt on write; call `soft_cons_repay()` or `polish()` before external reads. Ablate with `set_soft_cons(Off)`.
- **P2 Soft-on (pit 144)** — Hole-starve `starve_fit` + `starve_promote` in Soft defaults.
- **Peers baseline** — `backup/peers-post/PEERS_POST.md` (vs SQLite / LMDB; async write ~0.05–0.06×; sync+repay range can beat SQL).
- **Strict CI** — `main` does not `cancel-in-progress` (pit 152); multi-writer `maint_loop` serialized with `ml_mu_` (pit 153); `tsan_g17` timing + failure logs (pit 154).

## Upgrade notes

| From v0.1.0 Soft | Behavior |
|:---|:---|
| `configure_deploy` / Soft | Default `SoftCons::Post` (no longer “write-for-read live Soft” as the product story) |
| Read path | For stable range/get SLA: call `soft_cons_repay()` or `polish()` after writes |
| P2 | Soft enables starve knives by default; ablate with `set_part_starve_fit(false)` etc. |

API unchanged; keys remain `u64`; single-process.

## Build & test

```sh
make            # full regression
make ci         # local Strict CI without sanitizers
make asan       # ASan + UBSan + TSan
make peers-post # Soft Post peers (if peer DBs installed)
```

Entry: [`README.md`](../../README.md) · Eval: [`EVAL17.md`](EVAL17.md) · Contract: [`CONSTRAINTS.md`](../../CONSTRAINTS.md) · Prior: [v0.1.0](RELEASE_NOTES_v0.1.0.md) · Next: [v0.2.1](RELEASE_NOTES_v0.2.1.md)

## Known gaps (at v0.2.0 tag)

Single-process; fixed `u64` keys; no hour-scale soak / true power-loss contract; LMDB range ~2.5× in peers-post sync story (later eval-full short window ~1.2× after read knives—see v0.2.1); **at tag time** `fcs.h` was still monolithic (split landed in **v0.2.1** / pit 169); `get` CRC opt-in only.
