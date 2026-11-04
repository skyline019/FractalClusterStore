#!/usr/bin/env bash
# Soft peers 轻量延迟 A/B：stream_best_fit off/on，交错多轮压系统波动。
# 度量：insert / update / get / range us/op（bench4 Soft sync N=50k）。
# 输出：backup/soft-a1/latency_bf_ab.{csv,md}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/soft-a1}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-5}"
N="${N:-50000}"

echo "== build bench4 =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3

CSV="$OUT/latency_bf_ab.csv"
echo "round,arm,insert_us,update_us,get_us,range_us,max_commit_us,probes_per_get,hw,live,stream_best_fits" > "$CSV"

# Soft sync：sp_accel=0；argv[19]=stream_bf
# N B VSZ R S K kidx async gm lt am cons shr ithr kmode hwpct soft sp_accel bf
run_one() {
  local arm="$1" bf="$2" round="$3"
  local log="$OUT/latency_bf_r${round}_${arm}.log"
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  ./build/bench4 "$N" 500 32 "$N" 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 "$bf" \
    > "$log" 2>&1
  # phase 表： "fcs insert  5.207 ..."；对比表字段名含空格勿用 $2
  local ins upd get rng cmax probes hw
  ins=$(awk '/^fcs insert/{print $3; exit}' "$log")
  upd=$(awk '/^fcs update/{print $3; exit}' "$log")
  get=$(awk '/^fcs get/{print $3; exit}' "$log")
  rng=$(awk '/^fcs range /{print $3; exit}' "$log")
  cmax=$(awk '/max commit =/{for(i=1;i<=NF;i++) if($i=="="){print $(i+1); exit}}' "$log")
  probes=$(awk '/probes\/get=/{
    for(i=1;i<=NF;i++) if($i ~ /^probes\/get=/){split($i,a,"="); print a[2]; exit}
  }' "$log")
  hw=$(awk '/# fcs space:/{
    for(i=1;i<=NF;i++) if($i ~ /^hw=/){split($i,a,"="); print a[2]; exit}
  }' "$log")
  echo "$round,$arm,${ins},${upd},${get},${rng},${cmax},${probes},${hw},,$bf" >> "$CSV"
  echo "  r$round $arm insert=$ins update=$upd get=$get range=$rng max_commit=$cmax probes/get=$probes"
}

echo "== interleaved Soft sync N=$N rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  run_one off 0 "$r"
  run_one on 1 "$r"
done

python3 - <<'PY' "$CSV" "$OUT/latency_bf_ab.md"
import sys, statistics as st
csv, md = sys.argv[1], sys.argv[2]
rows = []
with open(csv, encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        p = line.strip().split(",")
        if len(p) < 10: continue
        d = dict(zip(hdr, p))
        try:
            rows.append({
                "arm": d["arm"],
                "insert": float(d["insert_us"]),
                "update": float(d["update_us"]),
                "get": float(d["get_us"]),
                "range": float(d["range_us"]),
                "cmax": float(d["max_commit_us"] or "nan"),
                "probes": float(d["probes_per_get"] or "nan"),
            })
        except ValueError:
            continue

def med(xs):
    xs = [x for x in xs if x == x]
    return st.median(xs) if xs else float("nan")

def span(xs):
    xs = [x for x in xs if x == x]
    if not xs: return float("nan"), float("nan")
    return min(xs), max(xs)

def arm_stats(arm):
    sub = [r for r in rows if r["arm"] == arm]
    out = {}
    for k in ("insert", "update", "get", "range", "cmax", "probes"):
        xs = [r[k] for r in sub]
        mn, mx = span(xs)
        out[k] = (med(xs), mn, mx, len(xs))
    return out

off, on = arm_stats("off"), arm_stats("on")
lines = [
    "# Soft sync · stream_best_fit 延迟 A/B（交错多轮）",
    "",
    f"源：`{csv}` · 度量 µs/op（median；span=min–max）· 越小越好。",
    "",
    "| 指标 | off median | on median | Δ% (on/off−1) | off span | on span |",
    "|------|-----------:|----------:|--------------:|----------|---------|",
]
for k, label in (
    ("insert", "insert us/op"),
    ("update", "update us/op"),
    ("get", "get us/op"),
    ("range", "range us/scan"),
    ("cmax", "max commit µs"),
    ("probes", "probes/get"),
):
    om, omn, omx, on_ = off[k]
    nm, nmn, nmx, nn = on[k]
    dlt = (nm / om - 1.0) * 100.0 if om and om == om and nm == nm else float("nan")
    lines.append(
        f"| {label} | {om:.3f} | {nm:.3f} | {dlt:+.1f}% | {omn:.3f}–{omx:.3f} | {nmn:.3f}–{nmx:.3f} |"
    )

# noise gate: if |Δ| < half of max arm span relative, call inconclusive
lines += [
    "",
    "## 判读规则",
    "",
    "- 交错 off/on，压瞬时负载漂移；看 **median**，span 表波动。",
    "- 若 |Δ%| 小于两臂相对半宽的较大者，标 **噪声内**；否则标升/降。",
    "",
]
verdicts = []
for k, label in (("insert", "写 insert"), ("update", "写 update"), ("get", "点读 get"), ("range", "区间 range")):
    om, omn, omx, _ = off[k]
    nm, nmn, nmx, _ = on[k]
    if not (om and nm and om == om and nm == nm):
        verdicts.append(f"- {label}：**无数据**")
        continue
    dlt = (nm / om - 1.0) * 100.0
    half_w = max((omx - omn) / om, (nmx - nmn) / nm) * 50.0  # half span as %
    if abs(dlt) <= max(half_w, 3.0):
        tag = "噪声内"
    elif dlt > 0:
        tag = "变慢"
    else:
        tag = "变快"
    verdicts.append(f"- {label}：median Δ **{dlt:+.1f}%** → **{tag}**（半宽阈值≈{max(half_w,3):.1f}%）")

lines += verdicts
lines += ["", f"轮次：off={off['insert'][3]} on={on['insert'][3]}", ""]
open(md, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo LATENCY_BF_AB_OK out="$OUT"
