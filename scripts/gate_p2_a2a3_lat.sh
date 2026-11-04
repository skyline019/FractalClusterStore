#!/usr/bin/env bash
# 坑 138/141：P2 both（starve_fit+promote，含税刀+锁外β）相对 Soft 默认关
# —— A2 tiny / A3 Soft peers / Soft sync 写延迟门。Soft A1 峰见 VERDICT_P2_LOCKLESS_BETA。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/soft-a1}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-9}"          # 坑 143：加轮压 Soft sync 休眠噪声
N_LAT="${N_LAT:-50000}"

echo "== build =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/tiny_space.cpp -o build/tiny_space
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3

A2CSV="$OUT/p2_a2.csv"
echo "pages,arm,peak_x,peak_tail,live,nospace,starve_fits,starve_promotes" > "$A2CSV"

run_a2() {
  local pages="$1" arm="$2" sf="$3" sp="$4"
  local log="$OUT/p2_a2_p${pages}_${arm}.log"
  echo "== A2 pages=$pages arm=$arm =="
  # pages accel N bf_mode bf_skip starve_fit starve_promote
  ./build/tiny_space "$pages" 0 0 0 0 "$sf" "$sp" 2>&1 | tee "$log"
  local px peak live nos sfits sprom
  px=$(grep -oP 'peak_x=\K[0-9.]+' "$log" | tail -1)
  peak=$(grep -oP 'peak_tail=\K[0-9]+' "$log" | tail -1)
  live=$(grep -oP 'live_pages=\K[0-9]+' "$log" | tail -1)
  nos=$(grep -oP 'nospace=\K[0-9]+' "$log" | tail -1)
  sfits=$(grep -oP 'starve_fits=\K[0-9]+' "$log" | tail -1)
  sprom=$(grep -oP 'starve_promotes=\K[0-9]+' "$log" | tail -1)
  echo "$pages,$arm,$px,$peak,$live,$nos,$sfits,$sprom" >> "$A2CSV"
}
for pg in 256 512 1024; do
  run_a2 "$pg" off 0 0
  run_a2 "$pg" both 1 1
done

A3CSV="$OUT/p2_a3.csv"
echo "arm,hw,live,peak_x,insert_us,update_us,get_us,range_us" > "$A3CSV"
run_a3() {
  local arm="$1" sf="$2" sp="$3"
  local log="$OUT/p2_a3_${arm}.log"
  echo "== A3 Soft peers N=50k arm=$arm =="
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  # N B VSZ R S K kidx async gm lt am cons shr ithr kmode hwpct soft sp_accel bf bf_skip starve_fit starve_promote
  ./build/bench4 50000 500 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 0 0 "$sf" "$sp" \
    2>&1 | tee "$log" | tail -30
  local hw live px ins upd get rng
  hw=$(grep -oP 'hw=\K[0-9]+' "$log" | tail -1 || echo 0)
  live=$(grep -oP 'live_pages=\K[0-9]+' "$log" | tail -1 || echo 1)
  [[ -z "$live" || "$live" == "0" ]] && live=1
  px=$(awk -v h="${hw:-0}" -v l="$live" 'BEGIN{printf "%.3f", h/l}')
  ins=$(awk '/^fcs insert/{print $3; exit}' "$log")
  upd=$(awk '/^fcs update/{print $3; exit}' "$log")
  get=$(awk '/^fcs get/{print $3; exit}' "$log")
  rng=$(awk '/^fcs range /{print $3; exit}' "$log")
  echo "$arm,${hw:-0},$live,$px,$ins,$upd,$get,$rng" >> "$A3CSV"
  echo "  A3 $arm hw=$hw live=$live peak_x=$px insert=$ins"
}
run_a3 off 0 0
run_a3 both 1 1

LATCSV="$OUT/latency_p2_ab.csv"
echo "round,arm,insert_us,update_us,get_us,range_us,max_commit_us,probes_per_get,starve_fits,starve_promotes,merge_parts" > "$LATCSV"
run_lat() {
  local arm="$1" sf="$2" sp="$3" round="$4"
  local log="$OUT/latency_p2_r${round}_${arm}.log"
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  ./build/bench4 "$N_LAT" 500 32 "$N_LAT" 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 0 0 "$sf" "$sp" \
    > "$log" 2>&1
  local ins upd get rng cmax probes fits prom parts
  ins=$(awk '/^fcs insert/{print $3; exit}' "$log")
  upd=$(awk '/^fcs update/{print $3; exit}' "$log")
  get=$(awk '/^fcs get/{print $3; exit}' "$log")
  rng=$(awk '/^fcs range /{print $3; exit}' "$log")
  cmax=$(awk '/max commit =/{for(i=1;i<=NF;i++) if($i=="="){print $(i+1); exit}}' "$log")
  probes=$(awk '/probes\/get=/{
    for(i=1;i<=NF;i++) if($i ~ /^probes\/get=/){split($i,a,"="); print a[2]; exit}
  }' "$log")
  fits=$(grep -oP 'starve_fits=\K[0-9]+' "$log" | tail -1)
  prom=$(grep -oP 'starve_promotes=\K[0-9]+' "$log" | tail -1)
  parts=$(grep -oP 'merge_parts=\K[0-9]+' "$log" | tail -1)
  echo "$round,$arm,$ins,$upd,$get,$rng,${cmax:-},$probes,${fits:-0},${prom:-0},${parts:-0}" >> "$LATCSV"
  echo "  lat r$round $arm insert=$ins update=$upd fits=${fits:-0} prom=${prom:-0}"
}
echo "== interleaved Soft sync latency N=$N_LAT rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  run_lat off 0 0 "$r"
  run_lat both 1 1 "$r"
done

python3 - <<'PY' "$OUT"
import statistics as st, pathlib, sys
out = pathlib.Path(sys.argv[1])

def med(xs):
    xs = [x for x in xs if x == x]
    return st.median(xs) if xs else float("nan")

def span(xs):
    xs = [x for x in xs if x == x]
    return (min(xs), max(xs)) if xs else (float("nan"), float("nan"))

lines = ["# P2 both · A2 / A3 / 写延迟门（坑 138/141 税刀后重跑）", "",
         "臂：`off` = Soft 默认关旋钮；`both` = starve_fit+starve_promote"
         "（FLOOR=32 · SLICES=12 · PROMOTES=1 · MIN_PEND=64 · 锁外 β）。",
         f"源：`{out}/p2_a2.csv` · `{out}/p2_a3.csv` · `{out}/latency_p2_ab.csv`", ""]

# A2
lines += ["## A2 tiny_space（accel=0）", "",
          "| pages | off peak_x | both peak_x | Δ% | 判 |",
          "|------:|----------:|------------:|---:|----|"]
a2 = {}
with open(out / "p2_a2.csv", encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        a2.setdefault(d["pages"], {})[d["arm"]] = float(d["peak_x"])
for pg in sorted(a2, key=int):
    o, b = a2[pg]["off"], a2[pg]["both"]
    dlt = (b / o - 1) * 100 if o else float("nan")
    if abs(dlt) < 1.0: tag = "平"
    elif dlt > 0: tag = "升（负）"
    else: tag = "降"
    lines.append(f"| {pg} | {o:.3f} | {b:.3f} | {dlt:+.1f}% | {tag} |")

# A3
lines += ["", "## A3 Soft peers（bench4 N=50k sync）", "",
          "| arm | hw | live | peak_x(hw/live) | insert | update | get | range |",
          "|-----|---:|-----:|----------------:|-------:|-------:|----:|------:|"]
with open(out / "p2_a3.csv", encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    a3 = {}
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        a3[d["arm"]] = d
        lines.append(
            f"| {d['arm']} | {d['hw']} | {d['live']} | {d['peak_x']} | "
            f"{d['insert_us']} | {d['update_us']} | {d['get_us']} | {d['range_us']} |"
        )
if "off" in a3 and "both" in a3:
    o, b = float(a3["off"]["peak_x"]), float(a3["both"]["peak_x"])
    dlt = (b / o - 1) * 100 if o else float("nan")
    lines += ["", f"A3 peak_x Δ **{dlt:+.1f}%**（both/off−1）。", ""]

# Latency
rows = []
with open(out / "latency_p2_ab.csv", encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        try:
            rows.append({k: (d[k] if k == "arm" else float(d[k] or "nan")) for k in hdr if k != "round"})
        except ValueError:
            continue

def arm_stats(arm):
    sub = [r for r in rows if r["arm"] == arm]
    outd = {}
    for k in ("insert_us", "update_us", "get_us", "range_us", "max_commit_us", "probes_per_get",
              "starve_fits", "starve_promotes", "merge_parts"):
        if k not in (sub[0] if sub else {}):
            continue
        xs = [r[k] for r in sub]
        mn, mx = span(xs)
        outd[k] = (med(xs), mn, mx, len(xs))
    return outd

off, both = arm_stats("off"), arm_stats("both")
both_fire = 0.0
if "starve_fits" in both and "starve_promotes" in both:
    both_fire = both["starve_fits"][0] + both["starve_promotes"][0]
dormant = both_fire == 0.0
lines += ["## Soft sync 写延迟（交错×轮 · median µs/op）", "",
          f"P2 开火（both 中位 fits+prom）= **{both_fire:.0f}**"
          f"{' → 休眠：写 Δ 倾向噪声' if dormant else ''}。", "",
          "| 指标 | off median | both median | Δ% | off span | both span | 判 |",
          "|------|-----------:|------------:|---:|----------|-----------|----|"]
verdicts = []
for k, label in (
    ("insert_us", "insert"),
    ("update_us", "update"),
    ("get_us", "get"),
    ("range_us", "range"),
    ("max_commit_us", "max commit"),
    ("probes_per_get", "probes/get"),
):
    om, omn, omx, _ = off[k]
    bm, bmn, bmx, _ = both[k]
    dlt = (bm / om - 1) * 100 if om == om and bm == bm and om else float("nan")
    half_w = max((omx - omn) / om, (bmx - bmn) / bm) * 50.0 if om and bm else 99
    # 坑 143：P2 全休眠时放宽写税门槛（单轮 Soft sync 本就不触发 α/β）
    floor = 8.0 if dormant and k in ("insert_us", "update_us") else 3.0
    if abs(dlt) <= max(half_w, floor):
        tag = "噪声内"
    elif dlt > 0:
        tag = "变慢"
    else:
        tag = "变快"
    lines.append(
        f"| {label} | {om:.3f} | {bm:.3f} | {dlt:+.1f}% | {omn:.3f}–{omx:.3f} | {bmn:.3f}–{bmx:.3f} | {tag} |"
    )
    if k in ("insert_us", "update_us", "get_us", "range_us"):
        verdicts.append(f"- {label}：Δ **{dlt:+.1f}%** → **{tag}**")

# Gate
a2_worse = any(
    (a2[pg]["both"] / a2[pg]["off"] - 1) * 100 > 1.0 for pg in a2
)
a3_dlt = (float(a3["both"]["peak_x"]) / float(a3["off"]["peak_x"]) - 1) * 100 if "off" in a3 and "both" in a3 else 0
write_tax = any("变慢" in v for v in verdicts if v.startswith("- insert") or v.startswith("- update"))
gate_ok = (not a2_worse) and a3_dlt <= 1.0 and not write_tax

lines += ["", "## 门禁判决", ""]
lines += verdicts
lines += [
    "",
    f"- A2：{'有档 peak_x 升 >1%' if a2_worse else '三档平/降（无 >1% 升）'}",
    f"- A3 peak_x Δ {a3_dlt:+.1f}%",
    f"- Soft A1（VERDICT_P2_LOCKLESS_BETA）：both 峰↓≈13%、insert≈+3%",
    f"- Soft sync P2：{'休眠（fits+prom=0）' if dormant else '有开火'}",
    f"- 写路径：{'insert/update 有「变慢」' if write_tax else 'insert/update 无「变慢」'}",
    "",
    f"**升 Soft 默认：{'可议（三画像+延迟同向）' if gate_ok else '否'}**",
    "",
]
(out / "VERDICT_P2_GATE.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
PY

echo P2_A2A3_LAT_OK out="$OUT"
