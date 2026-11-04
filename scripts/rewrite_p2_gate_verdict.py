#!/usr/bin/env python3
"""Rebuild VERDICT_P2_GATE.md from existing CSVs/logs (fixes A3 live_pages parse)."""
from __future__ import annotations

import re
import statistics as st
from pathlib import Path

out = Path("backup/soft-a1")

rows = []
for arm in ("off", "both"):
    log = (out / f"p2_a3_{arm}.log").read_text(errors="replace")
    hw = int(re.search(r"hw=(\d+)", log).group(1))
    live = int(re.search(r"live_pages=(\d+)", log).group(1))
    px = hw / live if live else 0.0
    ins = float(re.search(r"^fcs insert\s+([0-9.]+)", log, re.M).group(1))
    upd = float(re.search(r"^fcs update\s+([0-9.]+)", log, re.M).group(1))
    get = float(re.search(r"^fcs get\s+([0-9.]+)", log, re.M).group(1))
    rng = float(re.search(r"^fcs range\s+([0-9.]+)", log, re.M).group(1))
    rows.append((arm, hw, live, px, ins, upd, get, rng))

(out / "p2_a3.csv").write_text(
    "arm,hw,live,peak_x,insert_us,update_us,get_us,range_us\n"
    + "\n".join(
        f"{a},{h},{l},{p:.3f},{i},{u},{g},{r}" for a, h, l, p, i, u, g, r in rows
    )
    + "\n",
    encoding="utf-8",
)

a2: dict[str, dict[str, float]] = {}
with open(out / "p2_a2.csv", encoding="utf-8") as f:
    next(f)
    for line in f:
        pg, arm, px, *_ = line.strip().split(",")
        a2.setdefault(pg, {})[arm] = float(px)

lat_rows = []
with open(out / "latency_p2_ab.csv", encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        lat_rows.append(
            {
                k: (d[k] if k == "arm" else float(d[k] or "nan"))
                for k in hdr
                if k != "round"
            }
        )


def med(xs):
    xs = [x for x in xs if x == x]
    return st.median(xs) if xs else float("nan")


def span(xs):
    xs = [x for x in xs if x == x]
    return (min(xs), max(xs)) if xs else (float("nan"), float("nan"))


def arm_stats(arm):
    sub = [r for r in lat_rows if r["arm"] == arm]
    o = {}
    for k in (
        "insert_us",
        "update_us",
        "get_us",
        "range_us",
        "max_commit_us",
        "probes_per_get",
    ):
        xs = [r[k] for r in sub]
        mn, mx = span(xs)
        o[k] = (med(xs), mn, mx)
    return o


off, both = arm_stats("off"), arm_stats("both")

lines = [
    "# P2 both · A2 / A3 / 写延迟门（坑 138）",
    "",
    "臂：`off` vs `both`（starve_fit+starve_promote）。",
    "",
    "## A2 tiny_space（accel=0）",
    "",
    "| pages | off peak_x | both peak_x | Δ% | 判 |",
    "|------:|----------:|------------:|---:|----|",
]
a2_worse = False
for pg in sorted(a2, key=int):
    o, b = a2[pg]["off"], a2[pg]["both"]
    dlt = (b / o - 1) * 100
    tag = "平" if abs(dlt) < 1 else ("升（负）" if dlt > 0 else "降")
    if dlt > 1:
        a2_worse = True
    lines.append(f"| {pg} | {o:.3f} | {b:.3f} | {dlt:+.1f}% | {tag} |")

o3, b3 = rows[0], rows[1]
a3_dlt = (b3[3] / o3[3] - 1) * 100
lines += [
    "",
    "## A3 Soft peers（bench4 N=50k sync）",
    "",
    "| arm | hw | live | peak_x | insert | update | get | range |",
    "|-----|---:|-----:|-------:|-------:|-------:|----:|------:|",
    f"| off | {o3[1]} | {o3[2]} | {o3[3]:.3f} | {o3[4]} | {o3[5]} | {o3[6]} | {o3[7]} |",
    f"| both | {b3[1]} | {b3[2]} | {b3[3]:.3f} | {b3[4]} | {b3[5]} | {b3[6]} | {b3[7]} |",
    "",
    f"A3 peak_x Δ **{a3_dlt:+.1f}%**。",
    "",
    "## Soft sync 写延迟（交错×5 · median µs/op）",
    "",
    "| 指标 | off | both | Δ% | 判 |",
    "|------|---:|-----:|---:|----|",
]
verdicts = []
for k, label in (
    ("insert_us", "insert"),
    ("update_us", "update"),
    ("get_us", "get"),
    ("range_us", "range"),
):
    om, omn, omx = off[k]
    bm, bmn, bmx = both[k]
    dlt = (bm / om - 1) * 100
    half_w = max((omx - omn) / om, (bmx - bmn) / bm) * 50
    tag = "噪声内" if abs(dlt) <= max(half_w, 3) else ("变慢" if dlt > 0 else "变快")
    lines.append(f"| {label} | {om:.3f} | {bm:.3f} | {dlt:+.1f}% | {tag} |")
    verdicts.append(f"- {label}：Δ **{dlt:+.1f}%** → **{tag}**")

write_tax = any("变慢" in v for v in verdicts if "insert" in v or "update" in v)
gate_ok = (not a2_worse) and a3_dlt <= 1.0 and not write_tax
lines += [
    "",
    "## 门禁判决",
    "",
    *verdicts,
    "",
    f"- A2：{'有档升 >1%' if a2_worse else '三档平/降'}",
    f"- A3 peak_x Δ {a3_dlt:+.1f}%",
    "- Soft A1：both 峰↓≈8%（VERDICT_P2）",
    f"- 写路径：{'insert/update 有变慢' if write_tax else '无变慢'}",
    "",
    f"**升 Soft 默认：{'可议' if gate_ok else '否'}**",
    "",
    "实验臂仍保留：`set_part_starve_fit(true)` + `set_starve_promote(true)`。",
    "复跑：`make soft-a1-p2-gate`",
    "",
]
text = "\n".join(lines)
(out / "VERDICT_P2_GATE.md").write_text(text, encoding="utf-8")
print(text)
