#!/usr/bin/env python3
import csv, sys
from collections import Counter, defaultdict

base = sys.argv[1] if len(sys.argv) > 1 else "backup/frag-trace/n200k"
holes = list(csv.DictReader(open(f"{base}/holes.csv", encoding="utf-8")))
rows = list(csv.DictReader(open(f"{base}/timeline.csv", encoding="utf-8")))

def ii(x):
    try: return int(float(x))
    except Exception: return 0

def at(commit, tag=None):
    out = []
    for h in holes:
        if ii(h["commit"]) != commit:
            continue
        if tag and h["tag"] != tag:
            continue
        out.append(h)
    return out

def summarize(hs, label):
    by = defaultdict(list)
    for h in hs:
        by[h["kind"]].append((ii(h["p0"]), ii(h["np"]), ii(h["mid"])))
    print(f"\n=== {label} ===")
    for k, xs in by.items():
        sizes = sorted(n for _, n, _ in xs)
        print(
            f"{k}: segs={len(xs)} pages={sum(sizes)} max={max(sizes) if sizes else 0} "
            f"ge64={sum(1 for n in sizes if n >= 64)} lt64={sum(1 for n in sizes if n < 64)}"
        )
        print("  size hist:", Counter(sizes).most_common(10))
        if k == "FREE":
            for p0, n, mid in sorted(xs)[:20]:
                print(f"  FREE p0={p0} np={n} mid={mid}")
            if len(xs) > 20:
                print(f"  ... +{len(xs)-20} more")

summarize(at(547), "commit 547 (peak)")
summarize(at(65, "STARVE"), "c65 first STARVE")
summarize(at(515, "STARVE"), "c515 late STARVE")

starve = [r for r in rows if r["attr"] == "HOLE_STARVE"]
cs = [ii(r["commit"]) for r in starve]
print("\nstarve deltas:", [cs[i] - cs[i - 1] for i in range(1, len(cs))])
print("starve d_tail:", [ii(r["d_tail"]) for r in starve])
print("starve pend/segs:", [(ii(r["pend_pages"]), ii(r["pend_segs"])) for r in starve])
for r in starve[:8]:
    ps = ii(r["pend_segs"])
    if ps:
        print(f"c{r['commit']} avg_pend_seg={ii(r['pend_pages'])/ps:.1f}")

print("\n=== starve commits any-tag spatial ===")
for c in [65, 129, 515, 547]:
    hs = [h for h in holes if ii(h["commit"]) == c]
    print(f"commit {c} tags={dict(Counter(h['tag'] for h in hs))} n={len(hs)}")
    by = defaultdict(list)
    for h in hs:
        by[h["kind"]].append(ii(h["np"]))
    for k, xs in by.items():
        print(f"  {k}: segs={len(xs)} pages={sum(xs)} hist={Counter(xs).most_common(6)}")
