import re
from pathlib import Path
out = Path(__file__).resolve().parent

def b7(tag, r):
    t = (out / f"med_b7_{tag}_r{r}.log").read_text(errors="replace")
    fm = re.search(r"# fcs done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+)", t)
    lm = re.search(r"# lmdb done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+)", t)
    return {k: float(fm.group(i)) / float(lm.group(i)) for i, k in enumerate(["ins", "upd", "get", "rng"], 1)}

def b4(tag, r):
    t = (out / f"med_b4_{tag}_r{r}.log").read_text(errors="replace")
    def us(phase, who="fcs"):
        m = re.search(rf"^{who} {phase}\s+([0-9.]+)", t, re.M)
        return float(m.group(1))
    return {k: us(k) / us(k, "sqlite") for k in ["insert", "update", "get", "range"]}

for tag in ["sync_t1d0", "async_t1d0"]:
    print("===", tag, "LMDB ratios===")
    rows = [b7(tag, r) for r in range(1, 6)]
    for k in ["ins", "upd", "get", "rng"]:
        xs = sorted(x[k] for x in rows)
        print(f"  {k}: {[f'{v:.2f}' for v in xs]} med={xs[2]:.2f}")
for tag in ["sync_t1d0", "async_t1d0"]:
    print("===", tag, "SQL ratios===")
    rows = [b4(tag, r) for r in range(1, 6)]
    for k in ["insert", "update", "get", "range"]:
        xs = sorted(x[k] for x in rows)
        print(f"  {k}: {[f'{v:.2f}' for v in xs]} med={xs[2]:.2f}")
