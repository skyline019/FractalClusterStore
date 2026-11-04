#!/usr/bin/env python3
"""Plot Soft sync work rhythm — multi-panel SVG + HTML (pit 148)."""
import csv
import json
import math
import sys
from pathlib import Path


def load(root: Path):
    rows = list(csv.DictReader((root / "work_timeline.csv").open()))
    for r in rows:
        r["commit"] = int(r["commit"])
        r["commit_us"] = float(r["commit_us"])
        r["d_level_us"] = float(r["d_level_us"] or 0)
        r["runs"] = int(r["runs"])
        r["peak_tail"] = int(r["peak_tail"])
        r["phase"] = r["phase"]
    cus = sorted(r["commit_us"] for r in rows)
    n = len(cus)

    def pct(p):
        return cus[int(p * (n - 1))]

    p25, p50, p90 = pct(0.25), pct(0.50), pct(0.90)
    for r in rows:
        c = r["commit_us"]
        if c >= p90:
            r["rhythm"] = "PEAK"
        elif c >= p50:
            r["rhythm"] = "BUSY"
        elif c >= p25:
            r["rhythm"] = "WARM"
        else:
            r["rhythm"] = "VALLEY"
    return rows, {"p25": p25, "p50": p50, "p90": p90, "max": cus[-1]}


COLORS = {
    "PEAK": "#c0392b",
    "BUSY": "#e67e22",
    "WARM": "#d4a017",
    "VALLEY": "#1e8449",
}


def panel_scatter(rows, stats, x0, y0, w, h, title):
    commits = [r["commit"] for r in rows]
    xmin, xmax = min(commits), max(commits)
    ymin = max(min(r["commit_us"] for r in rows), 1)
    ymax = stats["max"] * 1.08

    def X(c):
        return x0 + (c - xmin) / (xmax - xmin) * w

    def Y(v):
        lv = math.log10(max(v, 1))
        lmin, lmax = math.log10(ymin), math.log10(ymax)
        return y0 + h - (lv - lmin) / (lmax - lmin) * h

    parts = [
        f'<text x="{x0}" y="{y0 - 8}" font-family="Segoe UI,sans-serif" font-size="13" '
        f'font-weight="600" fill="#1a1a1a">{title}</text>',
        f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="#fbfbfb" stroke="#ddd"/>',
    ]
    for key, yv, col, dash in [
        ("p50", stats["p50"], "#2980b9", "6 4"),
        ("p90", stats["p90"], "#c0392b", "6 4"),
        ("p25", stats["p25"], "#1e8449", "2 3"),
    ]:
        yy = Y(yv)
        parts.append(
            f'<line x1="{x0}" x2="{x0+w}" y1="{yy:.1f}" y2="{yy:.1f}" '
            f'stroke="{col}" stroke-dasharray="{dash}" stroke-width="1.2"/>'
        )
        parts.append(
            f'<text x="{x0+w+6}" y="{yy+4:.1f}" font-size="10" fill="{col}" '
            f'font-family="Segoe UI,sans-serif">{key} {yv/1000:.1f}ms</text>'
        )
    upd = next((r["commit"] for r in rows if r["phase"] == "upd"), None)
    if upd:
        parts.append(
            f'<line x1="{X(upd):.1f}" x2="{X(upd):.1f}" y1="{y0}" y2="{y0+h}" '
            f'stroke="#7f8c8d" stroke-dasharray="3 3"/>'
        )
        parts.append(
            f'<text x="{X(upd)+4:.1f}" y="{y0+14}" font-size="10" fill="#7f8c8d" '
            f'font-family="Segoe UI,sans-serif">upd</text>'
        )
    for r in rows:
        parts.append(
            f'<circle cx="{X(r["commit"]):.1f}" cy="{Y(r["commit_us"]):.1f}" r="2.4" '
            f'fill="{COLORS[r["rhythm"]]}"/>'
        )
    parts.append(
        f'<text x="{x0}" y="{y0+h+18}" font-size="10" fill="#666" '
        f'font-family="Segoe UI,sans-serif">commit # → · log commit_us</text>'
    )
    return "\n".join(parts)


def panel_bars(rows, x0, y0, w, h, key, title, color):
    commits = [r["commit"] for r in rows]
    xmin, xmax = min(commits), max(commits)
    vals = [r[key] for r in rows]
    vmax = max(vals) * 1.05 or 1

    def X(c):
        return x0 + (c - xmin) / (xmax - xmin) * w

    def Y(v):
        return y0 + h - (v / vmax) * h

    bw = max(1.0, w / len(rows))
    parts = [
        f'<text x="{x0}" y="{y0 - 8}" font-family="Segoe UI,sans-serif" font-size="13" '
        f'font-weight="600" fill="#1a1a1a">{title}</text>',
        f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="#fbfbfb" stroke="#ddd"/>',
    ]
    path = []
    for r in rows:
        x = X(r["commit"])
        y = Y(r[key])
        path.append(f"{x:.1f},{y:.1f}")
        parts.append(
            f'<rect x="{x - bw/2:.1f}" y="{y:.1f}" width="{bw:.1f}" '
            f'height="{y0+h-y:.1f}" fill="{color}" opacity="0.55"/>'
        )
    upd = next((r["commit"] for r in rows if r["phase"] == "upd"), None)
    if upd:
        parts.append(
            f'<line x1="{X(upd):.1f}" x2="{X(upd):.1f}" y1="{y0}" y2="{y0+h}" '
            f'stroke="#7f8c8d" stroke-dasharray="3 3"/>'
        )
    return "\n".join(parts)


def panel_dual(rows, x0, y0, w, h):
    commits = [r["commit"] for r in rows]
    xmin, xmax = min(commits), max(commits)
    runs = [r["runs"] for r in rows]
    tails = [r["peak_tail"] for r in rows]
    rmax, tmax = max(runs) * 1.1 or 1, max(tails) * 1.1 or 1

    def X(c):
        return x0 + (c - xmin) / (xmax - xmin) * w

    def Yr(v):
        return y0 + h - v / rmax * h

    def Yt(v):
        return y0 + h - v / tmax * h

    parts = [
        f'<text x="{x0}" y="{y0 - 8}" font-family="Segoe UI,sans-serif" font-size="13" '
        f'font-weight="600" fill="#1a1a1a">runs (blue) · peak_tail pages (teal)</text>',
        f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="#fbfbfb" stroke="#ddd"/>',
    ]
    pr = " ".join(f"{X(r['commit']):.1f},{Yr(r['runs']):.1f}" for r in rows)
    pt = " ".join(f"{X(r['commit']):.1f},{Yt(r['peak_tail']):.1f}" for r in rows)
    parts.append(f'<polyline fill="none" stroke="#2980b9" stroke-width="1.5" points="{pr}"/>')
    parts.append(f'<polyline fill="none" stroke="#16a085" stroke-width="1.5" points="{pt}"/>')
    upd = next((r["commit"] for r in rows if r["phase"] == "upd"), None)
    if upd:
        parts.append(
            f'<line x1="{X(upd):.1f}" x2="{X(upd):.1f}" y1="{y0}" y2="{y0+h}" '
            f'stroke="#7f8c8d" stroke-dasharray="3 3"/>'
        )
    return "\n".join(parts)


def write_svg(rows, stats, out: Path, title: str):
    W, H = 1100, 780
    body = [
        panel_scatter(rows, stats, 40, 70, 920, 180, "① commit 墙钟（色=节奏）"),
        panel_bars(rows, 40, 310, 920, 160, "d_level_us", "② 层合并增量 d_level_us", "#8e44ad"),
        panel_dual(rows, 40, 530, 920, 160),
    ]
    legend = (
        f'<text x="40" y="36" font-family="Segoe UI,sans-serif" font-size="16" '
        f'font-weight="700" fill="#111">{title}</text>'
        f'<text x="40" y="54" font-size="11" fill="#555" font-family="Segoe UI,sans-serif">'
        f'<tspan fill="{COLORS["VALLEY"]}">● VALLEY&lt;p25</tspan>  '
        f'<tspan fill="{COLORS["WARM"]}">● WARM</tspan>  '
        f'<tspan fill="{COLORS["BUSY"]}">● BUSY≥p50</tspan>  '
        f'<tspan fill="{COLORS["PEAK"]}">● PEAK≥p90</tspan>  · 灰虚线=upd 开始'
        f"</text>"
    )
    svg = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}">\n'
        f'<rect width="100%" height="100%" fill="#ffffff"/>\n'
        f"{legend}\n" + "\n".join(body) + "\n</svg>\n"
    )
    out.write_text(svg, encoding="utf-8")


def write_html(rows, stats, out: Path, title: str, svg_name: str):
    # compact series for inline chart script
    series = [
        {
            "c": r["commit"],
            "us": round(r["commit_us"], 1),
            "lvl": round(r["d_level_us"], 1),
            "runs": r["runs"],
            "pt": r["peak_tail"],
            "ph": r["phase"],
            "r": r["rhythm"],
        }
        for r in rows
    ]
    html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<title>{title}</title>
<style>
  body {{ margin: 0; font-family: "Segoe UI", system-ui, sans-serif; background: #f4f5f7; color: #1a1a1a; }}
  header {{ padding: 20px 28px 8px; }}
  h1 {{ margin: 0; font-size: 1.25rem; }}
  .meta {{ color: #666; font-size: 0.9rem; margin-top: 6px; }}
  .wrap {{ padding: 0 20px 32px; }}
  img {{ width: 100%; max-width: 1100px; background: #fff; border-radius: 8px;
         box-shadow: 0 1px 4px rgba(0,0,0,.08); }}
  .stats {{ display: flex; gap: 12px; flex-wrap: wrap; margin: 12px 0 18px; }}
  .chip {{ background: #fff; border-radius: 6px; padding: 8px 12px; font-size: 0.85rem;
           box-shadow: 0 1px 3px rgba(0,0,0,.06); }}
</style>
</head>
<body>
<header>
  <h1>{title}</h1>
  <div class="meta">Soft sync · 相对分位节奏 · 数据 work_timeline.csv</div>
  <div class="stats">
    <div class="chip">p25 {stats['p25']/1000:.1f} ms</div>
    <div class="chip">p50 {stats['p50']/1000:.1f} ms</div>
    <div class="chip">p90 {stats['p90']/1000:.1f} ms</div>
    <div class="chip">max {stats['max']/1000:.1f} ms</div>
    <div class="chip">commits {len(rows)}</div>
  </div>
</header>
<div class="wrap">
  <img src="{svg_name}" alt="work rhythm chart"/>
</div>
<script type="application/json" id="series">{json.dumps(series)}</script>
</body>
</html>
"""
    out.write_text(html, encoding="utf-8")


def main():
    roots = sys.argv[1:] or ["backup/sync-work/n50000", "backup/sync-work/n200000"]
    for root_s in roots:
        root = Path(root_s)
        rows, stats = load(root)
        title = f"Soft sync 工作节奏 — {root.name}"
        svg = root / "work_rhythm.svg"
        html = root / "work_rhythm.html"
        write_svg(rows, stats, svg, title)
        write_html(rows, stats, html, title, svg.name)
        print("wrote", svg)
        print("wrote", html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
