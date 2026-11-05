# Soft deploy recipe（收敛期唯一推荐接入）

Product Soft = `configure_deploy` + repay before reads.  
Charter: [`CONVERGE_OPS.md`](../30-discuss/CONVERGE_OPS.md) · pit **170**.

## When to use FCS

- Local ordered table with **`u64` keys** (device state, config index, log seq → payload)
- Single process owns the open store
- You care about write amp / space / point get more than mmap-style range

## Minimal loop

```cpp
#include "fcs.h"
using namespace fcs;

RamDev dev(1u << 16);          // or FileDev
Store s;
u8 arena[1024 * PAGE];
if (s.open(&dev, arena, 1024) != RC::OK) return 1;

s.configure_deploy(true);      // Soft + hole protect + P2 + Post + Soft read knives

// --- write epoch ---
for (u64 k = 0; k < 1000; ++k) {
    char v[32];
    // fill v …
    if (s.put(k, v, sizeof v) != RC::OK) return 1;
    if ((k + 1) % 100 == 0 && s.commit() != RC::OK) return 1;
}
if (s.commit() != RC::OK) return 1;

// --- before serving reads ---
s.soft_cons_repay();           // or s.polish() for a heavier tidy

u8 buf[64];
u16 n = 0;
s.get(42, buf, sizeof buf, &n);
```

Runnable sketch: [`examples/soft_u64_log.cpp`](../../examples/soft_u64_log.cpp) · `make soft-example`.

## Do / Don't

| Do | Don't |
|----|-------|
| One `configure_deploy` at open | Hand-tune Negate-keep knobs as “product” |
| `soft_cons_repay` / `polish` before external range/get SLA | Expect live Soft (no repay) range ≈ LMDB |
| Ablate with `set_soft_cons(Off)` only for A/B | Flip `dual_mem` / `space_accel` / coalesce B on by default |
| Keep `select`/`scan` callbacks free of `get`/`page` if `range_page_view` on | Chase short-window LMDB with more load knives (pit 167) |

## Frozen Soft surface (configure_deploy)

| Knob | Soft value | Notes |
|------|------------|-------|
| `soft_cons` | Post | pit 151 |
| `flush_tip` | on | pit 163 |
| `range_open_fast` / `range_page_view` / `range_oneseg_sel` | on | 158 / 160 / 164 |
| `range_pf_eager` | off | 161 |
| hole protect + P2 starve | on | 144 |
| `space_accel` / `dual_mem` / coalesce B | off | Negate or Soft-forbid |

Full list: `backup/work-dist/OPT_COMPLETE.md`.

## Ops gates

```sh
make ci-smoke
make test          # before any behavior change
make soak-smoke    # 30s; not a power-loss contract
# Optional long soak (converge ops): FCS_SOAK_SEC=3600 make soak
# Log under backup/converge/; still not a true power-loss matrix (f3/g6/h3).
```
