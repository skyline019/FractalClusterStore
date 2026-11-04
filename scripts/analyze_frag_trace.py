#!/usr/bin/env python3
"""Analyze frag_trace timeline.csv + holes.csv → stdout + analysis.json."""
import csv, json, statistics as st, sys
from collections import defaultdict, Counter

base = sys.argv[1] if len(sys.argv) > 1 else "backup/frag-trace/n200k"
rows = list(csv.DictReader(open(f"{base}/timeline.csv", encoding="utf-8")))
holes = list(csv.DictReader(open(f"{base}/holes.csv", encoding="utf-8")))

def ii(x):
    try: return int(float(x))
    except Exception: return 0

starve = [r for r in rows if r["attr"] == "HOLE_STARVE"]
choke = [r for r in rows if ii(r["n_ge64"]) == 0 and ii(r["free_tot"]) >= 64]
choke_pend = [r for r in choke if ii(r["pend_pages"]) >= 64]

peak_val = max(ii(r["peak"]) for r in rows)
peak_at = None
for r in rows:
    if ii(r["tail"]) == peak_val:
        peak_at = r
        break

def avg_hist(subset):
    cols = ["h1","h2","h4","h8","h16","h32","h64","h128","h256","h512","h1024","h2048p"]
    return {c: (st.mean([ii(r[c]) for r in subset]) if subset else 0) for c in cols}

print("=== TEMPORAL ===")
print(f"commits={len(rows)} starve={len(starve)} choke(n_ge64=0&free>=64)={len(choke)} choke+pend64={len(choke_pend)}")
print(f"peak={peak_val} at c{peak_at['commit'] if peak_at else '?'} phase={peak_at['phase'] if peak_at else '?'}")
if peak_at:
    print(
        f"  peak snap: maxh={peak_at['maxh']} n_ge64={peak_at['n_ge64']} free={peak_at['free_tot']} "
        f"pend={peak_at['pend_pages']}/{peak_at['pend_segs']} "
        f"L={peak_at['L0']}/{peak_at['L1']}/{peak_at['L2']}/{peak_at['L3']} attr={peak_at['attr']}"
    )

print("\nSTARVE commits:")
for r in starve:
    print(
        f"  c{r['commit']} {r['phase']} +{r['d_tail']} maxh={r['maxh']} free={r['free_tot']} "
        f"n_ge64={r['n_ge64']} pend={r['pend_pages']}/{r['pend_segs']} "
        f"L={r['L0']}/{r['L1']}/{r['L2']}/{r['L3']}"
    )

low = [r for r in rows if ii(r["maxh"]) < 64]
hi = [r for r in rows if ii(r["maxh"]) >= 64]
print(f"\nmaxh<64: n={len(low)} med_pend={st.median([ii(r['pend_pages']) for r in low]) if low else 0}")
print(f"maxh>=64: n={len(hi)} med_pend={st.median([ii(r['pend_pages']) for r in hi]) if hi else 0}")
print("starve by phase", dict(Counter(r["phase"] for r in starve)))
print("avg hist starve", {k: round(v, 2) for k, v in avg_hist(starve).items() if v})
print("avg hist all   ", {k: round(v, 2) for k, v in avg_hist(rows).items() if v})

ratios = []
for r in rows:
    ft, fg = ii(r["free_tot"]), ii(r["free_ge64"])
    if ft > 0:
        ratios.append(fg / ft)
print(
    f"free_ge64/free_tot mean={st.mean(ratios):.3f} med={st.median(ratios):.3f} min={min(ratios):.3f}"
    if ratios else "no free"
)

# series for charts: downsample every commit maxh, n_ge64, pend
series = []
for r in rows:
    series.append({
        "c": ii(r["commit"]),
        "phase": r["phase"],
        "maxh": ii(r["maxh"]),
        "n_ge64": ii(r["n_ge64"]),
        "pend": ii(r["pend_pages"]),
        "free": ii(r["free_tot"]),
        "tail": ii(r["tail"]),
        "starve": 1 if r["attr"] == "HOLE_STARVE" else 0,
        "d_tail": ii(r["d_tail"]),
    })

def spat(tag):
    hs = [h for h in holes if h["tag"] == tag]
    by = defaultdict(list)
    for h in hs:
        by[h["kind"]].append(ii(h["np"]))
    out = {}
    for k, xs in by.items():
        xs = sorted(xs)
        out[k] = {
            "n": len(xs), "pages": sum(xs), "max": max(xs) if xs else 0,
            "med": st.median(xs) if xs else 0,
            "ge64": sum(1 for x in xs if x >= 64),
            "lt64": sum(1 for x in xs if x < 64),
            "sizes": xs[:40],
        }
    return out

print("\n=== SPATIAL ===")
spatial = {}
for tag in ["PEAK", "STARVE", "END", "FRAG_BUMP", "GE64_DROP", "BIG", "SB_LAG"]:
    o = spat(tag)
    if not o:
        continue
    spatial[tag] = o
    print(f"\n{tag}:")
    for k, v in o.items():
        print(f"  {k}: segs={v['n']} pages={v['pages']} max={v['max']} med={v['med']} ge64={v['ge64']} lt64={v['lt64']}")

print("\nPEAK FREE holes:")
peak_free = []
for h in holes:
    if h["tag"] == "PEAK" and h["kind"] == "FREE":
        peak_free.append({"p0": ii(h["p0"]), "np": ii(h["np"]), "mid": ii(h["mid"])})
        print(f"  p0={h['p0']} np={h['np']} mid={h['mid']}")

print("\nPEAK PEND size hist:")
pc = Counter(ii(h["np"]) for h in holes if h["tag"] == "PEAK" and h["kind"] == "PEND")
peak_pend_hist = sorted(pc.items())
for np, c in peak_pend_hist:
    print(f"  np={np}: {c}")

# last starve free+pend
starve_cs = sorted({h["commit"] for h in holes if h["tag"] == "STARVE"}, key=int)
last_st = []
if starve_cs:
    sc = starve_cs[-1]
    print(f"\nLast STARVE c{sc}:")
    for h in holes:
        if h["commit"] == sc and h["tag"] == "STARVE":
            last_st.append({"kind": h["kind"], "p0": ii(h["p0"]), "np": ii(h["np"]), "mid": ii(h["mid"])})
            print(f"  {h['kind']} p0={h['p0']} np={h['np']} mid={h['mid']}")

# PEND segs at starve: typically many equal-size?
starve_pend_modes = []
for sc in starve_cs:
    xs = [ii(h["np"]) for h in holes if h["commit"] == sc and h["tag"] == "STARVE" and h["kind"] == "PEND"]
    if xs:
        starve_pend_modes.append({"commit": int(sc), "n": len(xs), "pages": sum(xs), "mode": Counter(xs).most_common(3)})

out = {
    "base": base,
    "commits": len(rows),
    "peak": peak_val,
    "peak_at": int(peak_at["commit"]) if peak_at else None,
    "starve_n": len(starve),
    "choke_n": len(choke),
    "choke_pend_n": len(choke_pend),
    "peak_snap": {k: peak_at[k] for k in ["maxh","n_ge64","free_tot","pend_pages","pend_segs","L0","L1","L2","L3","attr"]} if peak_at else {},
    "starve_timeline": [
        {"c": int(r["commit"]), "phase": r["phase"], "d": ii(r["d_tail"]), "maxh": ii(r["maxh"]),
         "pend": ii(r["pend_pages"]), "psegs": ii(r["pend_segs"]), "free": ii(r["free_tot"])}
        for r in starve
    ],
    "series": series,
    "spatial": {t: {k: {kk: vv for kk, vv in v.items() if kk != "sizes"} for k, v in o.items()} for t, o in spatial.items()},
    "peak_free": peak_free,
    "peak_pend_hist": [{"np": a, "n": b} for a, b in peak_pend_hist],
    "last_starve": last_st,
    "starve_pend_modes": starve_pend_modes,
    "ratio_ge64": {
        "mean": st.mean(ratios) if ratios else None,
        "med": st.median(ratios) if ratios else None,
        "min": min(ratios) if ratios else None,
    },
}
open(f"{base}/analysis.json", "w", encoding="utf-8").write(json.dumps(out, indent=2))
print(f"\nwrote {base}/analysis.json")
