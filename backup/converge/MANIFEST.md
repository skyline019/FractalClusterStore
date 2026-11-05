# Converge / ops artifacts

- Mode: `docs/30-discuss/CONVERGE_OPS.md` (ACTIVE · pits 170–171)
- Soft recipe: `docs/00-entry/SOFT_RECIPE.md`
- Example: `make soft-example`

## Soak

| File | Budget | Notes |
|------|--------|-------|
| `soak_3600_20261105.log` | 3600 s | **PASS** — ops=745228 commits=106694 verifies=372 runs=19; Soft + `set_verify_reads(true)`; **not** a power-loss contract (pit **172**) |

Repro: `FCS_SOAK_SEC=3600 make soak`

Also: `scripts/ablate_range_167.sh` is doc-only under converge (API removed; pit 171).
