#!/usr/bin/env bash
# Harness A/B 空间峰：A1 layout · A2 tiny(256/512/1024) · A3 Soft peers 轻片段
# 臂：space_accel=ON (Soft 默认 85) vs 消融 0。不做全量 make asan。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${HARNESS_OUT:-backup/harness-ab-space}"
mkdir -p "$OUT" build
CSV="$OUT/results.csv"
echo "profile,arm,N,pages,peak_tail,live,peak_x,space_accels,hw_bytes,note" > "$CSV"

HOST=$(uname -a | tr '\n' ' ')
{
  echo "# host: $HOST"
  echo "# date: $(date -Iseconds)"
  echo "# knife: Soft protect_holes + space_accel (pit 125); ablate=0"
  echo "# skip: full make asan (per user)"
} | tee "$OUT/META.txt"

echo "== build probes =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/layout.cpp -o build/layout
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/tiny_space.cpp -o build/tiny_space
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
echo BUILD_OK | tee -a "$OUT/META.txt"

append_row() { echo "$*" >> "$CSV"; }

# --- A1 layout 200k：protect+hw150 ± space_accel ---
# argv: N B VSZ mode quiet compact rpct protect budget units frag coal sp_accel
run_a1() {
  local arm="$1" accel="$2"
  local log="$OUT/a1_${arm}.log"
  echo "== A1 layout 200k arm=$arm accel=$accel =="
  ./build/layout 200000 500 32 1 1 0 150 1 -1 0 -1 -1 "$accel" 2>&1 | tee "$log" | tail -3
  local peak live accels
  peak=$(grep -oP 'peak_tail=\K[0-9]+' "$log" | tail -1 || echo 0)
  live=$(grep -oP 'live_pages=\K[0-9]+' "$log" | tail -1 || echo 1)
  accels=$(grep -oP 'space_accels=\K[0-9]+' "$log" | tail -1 || echo 0)
  local px
  px=$(awk -v p="$peak" -v l="$live" 'BEGIN{ if(l<1)l=1; printf "%.3f", p/l }')
  append_row "A1,$arm,200000,,$peak,$live,$px,$accels,,protect+hw150"
}
run_a1 soft_accel 85
run_a1 soft_ablate 0

# --- A2 tiny_space 三档 × 两臂 ---
run_a2() {
  local pages="$1" accel="$2"
  local arm
  if [[ "$accel" == "0" ]]; then arm=soft_ablate; else arm=soft_accel; fi
  local log="$OUT/a2_p${pages}_${arm}.log"
  echo "== A2 tiny pages=$pages arm=$arm =="
  ./build/tiny_space "$pages" "$accel" 2>&1 | tee "$log"
  local peak live px accels N
  peak=$(grep -oP 'peak_tail=\K[0-9]+' "$log" | tail -1 || echo 0)
  live=$(grep -oP 'live_pages=\K[0-9]+' "$log" | tail -1 || echo 1)
  px=$(grep -oP 'peak_x=\K[0-9.]+' "$log" | tail -1 || echo 0)
  accels=$(grep -oP 'space_accels=\K[0-9]+' "$log" | tail -1 || echo 0)
  N=$(grep -oP 'N=\K[0-9]+' "$log" | tail -1 || echo 0)
  append_row "A2,$arm,$N,$pages,$peak,$live,$px,$accels,,tiny_space"
}
for pg in 256 512 1024; do
  run_a2 "$pg" 85
  run_a2 "$pg" 0
done

# --- A3 Soft peers 轻片段 N=50k sync ± accel ---
BDIR="${FCS_BENCH_DIR:-${HOME:-/tmp}/.fcs_harness}"
mkdir -p "$BDIR"
run_a3() {
  local arm="$1" accel="$2"
  local log="$OUT/a3_${arm}.log"
  echo "== A3 bench4 Soft N=50k arm=$arm accel=$accel =="
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  # soft=1；末位 sp_accel
  ./build/bench4 50000 500 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0 1 "$accel" \
    2>&1 | tee "$log" | tail -25
  local hw live_b accels
  hw=$(grep -oP 'hw=\K[0-9]+' "$log" | tail -1 || echo 0)
  live_b=$(grep -oP 'live_pages=\K[0-9]+' "$log" | tail -1 || echo 1)
  accels=$(grep -oP 'space_accels=\K[0-9]+' "$log" | tail -1 || echo 0)
  local px
  px=$(awk -v h="$hw" -v l="$live_b" 'BEGIN{ if(l<1)l=1; printf "%.3f", h/l }')
  append_row "A3,$arm,50000,,,$live_b,$px,$accels,$hw,bench4_soft_sync"
}
run_a3 soft_accel 85
run_a3 soft_ablate 0

echo "== CSV =="
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
echo "HARNESS_AB_SPACE_OK out=$OUT"
