#!/usr/bin/env bash
# 坑 135：stream_best_fit 模式消融矩阵（峰 Soft A1 + 延迟 Soft sync×3 交错）
# arms: off | full | small | full_s180 | small_s180
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/soft-a1}"
mkdir -p "$OUT" build
N="${N:-200000}"
LAT_N="${LAT_N:-50000}"
LAT_ROUNDS="${LAT_ROUNDS:-3}"

echo "== build =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/soft_a1.cpp -o build/soft_a1
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
g++ -std=c++17 -O2 -g -pthread -Iinclude tests/c5.cpp -o build/c5
./build/c5

PEAK_CSV="$OUT/bf_modes_peak.csv"
echo "arm,bf_mode,bf_skip,peak_tail,peak_x,end_x,hole_starve,stream_best_fits,hw_reclaims" > "$PEAK_CSV"

run_peak() {
  local arm="$1" mode="$2" skip="$3"
  local log="$OUT/bf_peak_${arm}.log"
  echo "== Soft A1 peak arm=$arm mode=$mode skip=$skip =="
  ./build/soft_a1 "$N" 500 32 1 0 0 "$mode" "$skip" > "$log" 2>&1
  local peak px ex starve fits hw
  peak=$(grep -a '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_tail=/){split($i,a,"=");print a[2]}}')
  px=$(grep -a '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  ex=$(grep -a '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^end_x=/){split($i,a,"=");print a[2]}}')
  starve=$(grep -a '^== ATTR_COUNTS' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^HOLE_STARVE=/){split($i,a,"=");print a[2]}}')
  fits=$(grep -a '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^stream_best_fits=/){split($i,a,"=");print a[2]}}')
  hw=$(grep -a '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^hw_reclaims=/){split($i,a,"=");print a[2]}}')
  echo "$arm,$mode,$skip,$peak,$px,$ex,$starve,$fits,$hw" >> "$PEAK_CSV"
  echo "  -> peak_x=$px peak_tail=$peak fits=$fits starve=$starve"
}

run_peak off 0 0
run_peak full 1 0
run_peak small 2 0
run_peak full_s180 1 180
run_peak small_s180 2 180

LAT_CSV="$OUT/bf_modes_lat.csv"
echo "round,arm,bf_mode,bf_skip,insert_us,update_us,get_us,range_us,max_commit_us" > "$LAT_CSV"

run_lat_one() {
  local arm="$1" mode="$2" skip="$3" round="$4"
  local log="$OUT/bf_lat_r${round}_${arm}.log"
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  ./build/bench4 "$LAT_N" 500 32 "$LAT_N" 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 "$mode" "$skip" \
    > "$log" 2>&1
  local ins upd get rng cmax
  ins=$(awk '/^fcs insert/{print $3; exit}' "$log")
  upd=$(awk '/^fcs update/{print $3; exit}' "$log")
  get=$(awk '/^fcs get/{print $3; exit}' "$log")
  rng=$(awk '/^fcs range /{print $3; exit}' "$log")
  cmax=$(awk '/max commit =/{for(i=1;i<=NF;i++) if($i=="="){print $(i+1); exit}}' "$log")
  echo "$round,$arm,$mode,$skip,$ins,$upd,$get,$rng,$cmax" >> "$LAT_CSV"
  echo "  lat r$round $arm insert=$ins get=$get"
}

echo "== latency interleaved rounds=$LAT_ROUNDS =="
for r in $(seq 1 "$LAT_ROUNDS"); do
  run_lat_one off 0 0 "$r"
  run_lat_one full 1 0 "$r"
  run_lat_one small 2 0 "$r"
  run_lat_one full_s180 1 180 "$r"
  run_lat_one small_s180 2 180 "$r"
done

python3 - <<'PY' "$PEAK_CSV" "$LAT_CSV" "$OUT/VERDICT_P1d.md"
import sys, statistics as st, csv
peak_p, lat_p, md = sys.argv[1], sys.argv[2], sys.argv[3]
peaks = list(csv.DictReader(open(peak_p, encoding="utf-8")))
lats = list(csv.DictReader(open(lat_p, encoding="utf-8")))

def f(x):
    try: return float(x)
    except: return float("nan")

off_px = f(next(p["peak_x"] for p in peaks if p["arm"]=="off"))
lines = [
  "# P1d stream_best_fit 模式消融判决",
  "",
  "臂：`off` / `full`(mode1) / `small`(mode2) / `*_s180`(近满旁路 180)。exact 早停已烘焙。",
  f"峰：Soft A1 N=200k · 延迟：Soft sync N=50k 交错×轮。",
  "",
  "## Soft A1 峰",
  "",
  "| arm | mode | skip | peak_x | Δ% vs off | peak_tail | fits | HOLE_STARVE |",
  "|-----|-----:|-----:|-------:|----------:|----------:|-----:|------------:|",
]
for p in peaks:
  px = f(p["peak_x"])
  dlt = (px/off_px - 1)*100 if off_px==off_px and off_px else float("nan")
  lines.append(
    f"| {p['arm']} | {p['bf_mode']} | {p['bf_skip']} | {px:.3f} | {dlt:+.1f}% | {p['peak_tail']} | {p['stream_best_fits']} | {p['hole_starve']} |"
  )

lines += ["", "## Soft sync 延迟（median µs/op）", "",
  "| arm | insert | update | get | range | insert Δ% vs off |",
  "|-----|-------:|-------:|----:|------:|-----------------:|"]

arms = []
for a in ("off","full","small","full_s180","small_s180"):
  sub = [r for r in lats if r["arm"]==a]
  if not sub: continue
  def med(k):
    xs=[f(r[k]) for r in sub]; xs=[x for x in xs if x==x]
    return st.median(xs) if xs else float("nan")
  arms.append((a, med("insert_us"), med("update_us"), med("get_us"), med("range_us")))

off_ins = next((x[1] for x in arms if x[0]=="off"), float("nan"))
for a,ins,upd,get,rng in arms:
  dlt = (ins/off_ins - 1)*100 if off_ins==off_ins and off_ins else float("nan")
  lines.append(f"| {a} | {ins:.3f} | {upd:.3f} | {get:.3f} | {rng:.3f} | {dlt:+.1f}% |")

# score: prefer peak drop, penalize insert regress; Soft default still needs A2 but pick experimental best
cands = []
for p in peaks:
  if p["arm"]=="off": continue
  px = f(p["peak_x"])
  peak_gain = (off_px - px) / off_px * 100 if off_px else 0
  ins = next((x[1] for x in arms if x[0]==p["arm"]), float("nan"))
  ins_tax = (ins/off_ins - 1)*100 if off_ins and ins==ins else 0
  # objective: maximize peak_gain - 0.5*max(ins_tax,0) ; get must not blow
  getm = next((x[3] for x in arms if x[0]==p["arm"]), float("nan"))
  off_get = next((x[3] for x in arms if x[0]=="off"), float("nan"))
  get_tax = (getm/off_get - 1)*100 if off_get and getm==getm else 0
  score = peak_gain - 0.5*max(ins_tax,0) - 0.25*max(get_tax,0)
  cands.append((score, peak_gain, ins_tax, get_tax, p["arm"], px, ins))

cands.sort(reverse=True)
lines += ["", "## 综合（实验臂）", "",
  "得分 = 峰降% − 0.5·max(insert税%,0) − 0.25·max(get税%,0)", ""]
if cands:
  best = cands[0]
  lines.append(f"**最优实验臂：`{best[4]}`**（score={best[0]:.2f} · 峰降 {best[1]:.1f}% · insert税 {best[2]:+.1f}% · get税 {best[3]:+.1f}% · peak_x={best[5]:.3f}）")
  lines.append("")
  lines.append("Soft 默认：**仍关**（升默认仍须 A2/A3 同向 + 延迟噪声门）。")
  lines.append("")
  lines.append("| arm | score | 峰降% | insert税% | get税% |")
  lines.append("|-----|------:|------:|----------:|-------:|")
  for score, pg, it, gt, arm, px, ins in cands:
    lines.append(f"| {arm} | {score:.2f} | {pg:.1f} | {it:+.1f} | {gt:+.1f} |")

open(md,"w",encoding="utf-8").write("\n".join(lines)+"\n")
print("\n".join(lines))
PY

echo ABLATE_BF_MODES_OK out="$OUT"
