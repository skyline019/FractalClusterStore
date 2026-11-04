#!/usr/bin/env bash
# 坑 149：写税刀 — min_gap × run_hi
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt/root build
OUT=backup/range-opt/root
LOG=$OUT/idle_write_tax.log
CSV=$OUT/idle_write_tax.csv
: > "$LOG"
echo "arm,N,gap,run_hi,runs,peak_x,insert,update,range,cursors,tidies" > "$CSV"
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_idle_tidy.cpp -o build/range_idle_tidy

run_one() {
  local N=$1 arm=$2 gap=${3:-16} rh=${4:-16}
  echo "== N=$N arm=$arm gap=$gap run_hi=$rh ==" | tee -a "$LOG"
  local line
  line=$(./build/range_idle_tidy "$N" 500 2000 100 "$arm" 8000 5 "$gap" "$rh")
  echo "$line" | tee -a "$LOG"
  local a g rhv runs px ins upd rng cur it
  a=$(echo "$line" | awk '{split($1,a,"=");print a[2]}')
  g=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^gap=/){split($i,a,"=");print a[2]}}')
  rhv=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^run_hi=/){split($i,a,"=");print a[2]}}')
  runs=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^runs=/){split($i,a,"=");print a[2]}}')
  px=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  ins=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^insert=/){split($i,a,"=");print a[2]}}')
  upd=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^update=/){split($i,a,"=");print a[2]}}')
  rng=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^range=/){split($i,a,"=");print a[2]}}')
  cur=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^cursors=/){split($i,a,"=");print a[2]}}')
  it=$(echo "$line" | awk '{for(i=1;i<=NF;i++)if($i~/^tidies=/){split($i,a,"=");print a[2]}}')
  echo "$a,$N,$g,$rhv,$runs,$px,$ins,$upd,$rng,$cur,$it" >> "$CSV"
}

for N in 50000 200000; do
  run_one "$N" 0
  run_one "$N" 1
  # Idle matrix
  run_one "$N" 2 16 16
  run_one "$N" 2 32 16
  run_one "$N" 2 32 24
  run_one "$N" 2 48 24
  # Post leading
  run_one "$N" 3 32 16
  run_one "$N" 4
done
echo "--- csv ---" | tee -a "$LOG"
cat "$CSV" | tee -a "$LOG"
