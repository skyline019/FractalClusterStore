#!/usr/bin/env python3
"""Emit compact JSON series for frag-distribution canvas / VERDICT."""
import csv
import json
import sys
from collections import Counter
from pathlib import Path


def ii(x):
    try:
        return int(float(x))
    except Exception:
        return 0


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "backup/frag-trace/n200k")
    rows = list(csv.DictReader(open(d / "timeline.csv")))
    holes = list(csv.DictReader(open(d / "holes.csv")))

    series = []
    for r in rows[::8]:
        ft = ii(r["free_tot"])
        g = ii(r["free_ge64"])
        series.append(
            {
                "c": ii(r["commit"]),
                "ratio": round((g / ft) if ft else 0.0, 3),
                "maxh": ii(r["maxh"]),
                "free": ft,
                "pend": ii(r["pend_pages"]),
                "peak": ii(r["peak"]),
            }
        )

    starve = [r for r in rows if "STARVE" in (r.get("attr") or "")]
    peak_c = max(rows, key=lambda r: ii(r["peak"]))
    pc = ii(peak_c["commit"])
    peak_free = [
        ii(h["np"])
        for h in holes
        if ii(h["commit"]) == pc and h["kind"] == "FREE" and h["tag"] == "PEAK"
    ]
    hist = Counter(peak_free)

    # starve spatial (any tag — PEAK may overlay)
    starve_spatial = []
    for r in starve[:8]:
        c = ii(r["commit"])
        hs = [h for h in holes if ii(h["commit"]) == c]
        by = {}
        for h in hs:
            by.setdefault(h["kind"], []).append(ii(h["np"]))
        starve_spatial.append(
            {
                "c": c,
                "free": by.get("FREE", []),
                "pend": by.get("PEND", []),
                "pend_pages": ii(r["pend_pages"]),
                "pend_segs": ii(r["pend_segs"]),
                "free_tot": ii(r["free_tot"]),
                "maxh": ii(r["maxh"]),
                "d_tail": ii(r["d_tail"]),
            }
        )

    out = {
        "commits": len(rows),
        "peak": ii(peak_c["peak"]),
        "peak_commit": pc,
        "starve_n": len(starve),
        "choke": sum(1 for r in rows if ii(r["n_ge64"]) == 0 and ii(r["free_tot"]) >= 64),
        "maxh_lt64": sum(1 for r in rows if ii(r["maxh"]) < 64),
        "ratio_med": None,
        "series": series,
        "peak_free_hist": sorted(hist.items()),
        "peak_free_segs": len(peak_free),
        "peak_free_pages": sum(peak_free),
        "peak_free_max": max(peak_free) if peak_free else 0,
        "starve_spatial": starve_spatial,
        "starve_deltas": [
            ii(starve[i]["commit"]) - ii(starve[i - 1]["commit"])
            for i in range(1, len(starve))
        ],
    }
    ratios = []
    for r in rows:
        ft = ii(r["free_tot"])
        if ft:
            ratios.append(ii(r["free_ge64"]) / ft)
    ratios.sort()
    out["ratio_med"] = round(ratios[len(ratios) // 2], 3) if ratios else 0

    print(json.dumps(out, indent=2))
    (d / "chart_data.json").write_text(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
