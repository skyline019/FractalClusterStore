#!/usr/bin/env python3
"""Soft sync 工作节奏：相对分位峰/谷（坑 148）"""
import csv
import sys
from collections import Counter
from pathlib import Path


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "backup/sync-work")
    path = root / "work_timeline.csv"
    if not path.exists():
        print("missing", path)
        return 1
    rows = list(csv.DictReader(path.open()))
    if not rows:
        print("empty")
        return 1

    for r in rows:
        r["_cus"] = float(r["commit_us"])
        r["_lvl"] = float(r["d_level_us"] or 0)
        r["_units"] = int(float(r["d_units"] or 0))

    cus_s = sorted(r["_cus"] for r in rows)

    def pct(p):
        return cus_s[int(p * (len(cus_s) - 1))]

    p25, p50, p75, p90, p95, p99 = (
        pct(0.25),
        pct(0.50),
        pct(0.75),
        pct(0.90),
        pct(0.95),
        pct(0.99),
    )

    # 相对节奏标签（相对本跑分位，避免绝对 µs 门槛失效）
    for r in rows:
        c = r["_cus"]
        if c >= p90 or c >= max(3 * p50, p95):
            r["rhythm"] = "PEAK"
        elif c >= p50:
            r["rhythm"] = "BUSY"
        elif c >= p25:
            r["rhythm"] = "WARM"
        else:
            r["rhythm"] = "VALLEY"

    attrs = Counter(r["rhythm"] for r in rows)

    def streaks(label):
        out, cur, start = [], 0, None
        for i, r in enumerate(rows):
            if r["rhythm"] == label:
                if cur == 0:
                    start = i
                cur += 1
            elif cur:
                out.append((start + 1, cur, rows[start]["phase"]))
                cur = 0
        if cur:
            out.append((start + 1, cur, rows[start]["phase"]))
        return out

    peak_st = streaks("PEAK")
    valley_st = streaks("VALLEY")

    peaks = [r for r in rows if r["rhythm"] == "PEAK"]
    valleys = [r for r in rows if r["rhythm"] == "VALLEY"]

    n = len(rows)
    lines = []
    lines.append(f"commits={n} phases={dict(Counter(r['phase'] for r in rows))}")
    lines.append(
        f"commit_us p25={p25:.1f} p50={p50:.1f} p75={p75:.1f} "
        f"p90={p90:.1f} p95={p95:.1f} p99={p99:.1f} max={cus_s[-1]:.1f}"
    )
    lines.append(f"rhythm_counts={dict(attrs)}")
    lines.append(
        f"PEAK={len(peaks)} ({100*len(peaks)/n:.1f}%)  "
        f"VALLEY={len(valleys)} ({100*len(valleys)/n:.1f}%)"
    )

    top = sorted(peaks, key=lambda r: -r["_cus"])[:10]
    lines.append("top_work_peaks:")
    for r in top:
        lines.append(
            f"  c{r['commit']} {r['phase']} commit={r['commit_us']}us "
            f"level={r['d_level_us']}us units={r['d_units']} "
            f"runs={r['runs']} fits={r['d_fits']} prom={r['d_prom']}"
        )

    # 谷：最短 commit + 维护增量≈0
    deep = sorted(valleys, key=lambda r: (r["_cus"], r["_lvl"]))[:8]
    lines.append("deepest_valleys (low commit_us):")
    for r in deep:
        lines.append(
            f"  c{r['commit']} {r['phase']} commit={r['commit_us']}us "
            f"level={r['d_level_us']} units={r['d_units']} runs={r['runs']}"
        )

    lines.append(
        f"peak_streaks={len(peak_st)} longest={max((x[1] for x in peak_st), default=0)}"
    )
    if peak_st:
        lines.append(
            "longest_peak_streaks (start,len,phase): "
            + str(sorted(peak_st, key=lambda x: -x[1])[:5])
        )
    lines.append(
        f"valley_streaks={len(valley_st)} longest={max((x[1] for x in valley_st), default=0)}"
    )
    if valley_st:
        lines.append(
            "longest_valley_streaks (start,len,phase): "
            + str(sorted(valley_st, key=lambda x: -x[1])[:5])
        )

    # 四分位 + 相位
    for name, lo, hi in [
        ("Q1", 0, n // 4),
        ("Q2", n // 4, n // 2),
        ("Q3", n // 2, 3 * n // 4),
        ("Q4", 3 * n // 4, n),
    ]:
        chunk = rows[lo:hi]
        if not chunk:
            continue
        avg = sum(r["_cus"] for r in chunk) / len(chunk)
        lvl = sum(r["_lvl"] for r in chunk)
        rc = Counter(r["rhythm"] for r in chunk)
        lines.append(
            f"{name} [{lo+1}..{hi}] avg_us={avg:.1f} sum_level_us={lvl:.0f} rhythm={dict(rc)}"
        )

    for ph in ("ins", "upd", "drain", "fin"):
        chunk = [r for r in rows if r["phase"] == ph]
        if not chunk:
            continue
        avg = sum(r["_cus"] for r in chunk) / len(chunk)
        lvl = sum(r["_lvl"] for r in chunk)
        lines.append(
            f"phase_{ph}: n={len(chunk)} avg_us={avg:.1f} sum_level_us={lvl:.0f} "
            f"peak_share={sum(1 for r in chunk if r['rhythm']=='PEAK')/len(chunk):.2f}"
        )

    # 工作来源：level_us 占 commit 峰的比例
    peak_lvl = sum(r["_lvl"] for r in peaks)
    peak_cmt = sum(r["_cus"] for r in peaks) or 1
    lines.append(
        f"among_PEAK: sum_level_us/sum_commit_us={peak_lvl/peak_cmt:.2f} "
        f"(level merge share of peak wall)"
    )

    text = "\n".join(lines) + "\n"
    out = root / "work_rhythm.txt"
    out.write_text(text, encoding="utf-8")
    print(text)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
