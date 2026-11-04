#!/usr/bin/env bash
# 动态整理策略消融（坑 147 续）
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt/root build
OUT=backup/range-opt/root
LOG=$OUT/dyn_tidy.log
CSV=$OUT/dyn_tidy.csv
: > "$LOG"
echo "arm,N,runs,peak_used,live,peak_x,insert,update,range,cursors,steps" > "$CSV"
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_dyn_tidy.cpp -o build/range_dyn_tidy

run_one() {
  local N=$1 arm=$2
  echo "== N=$N arm=$arm ==" | tee -a "$LOG"
  local line
  line=$(./build/range_dyn_tidy "$N" 500 2000 100 "$arm")
  echo "$line" | tee -a "$LOG"
  local a runs pu live px ins upd rng cur st
  a=$(echo "$line" | awk '{split($1,a,"="); print a[2]}')
  runs=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^runs=/){split($i,a,"=");print a[2]}}')
  pu=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_used=/){split($i,a,"=");print a[2]}}')
  live=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^live=/){split($i,a,"=");print a[2]}}')
  px=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  ins=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^insert=/){split($i,a,"=");print a[2]}}')
  upd=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^update=/){split($i,a,"=");print a[2]}}')
  rng=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^range=/){split($i,a,"=");print a[2]}}')
  cur=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^cursors=/){split($i,a,"=");print a[2]}}')
  st=$(echo "$line" | awk '{for(i=1;i<=NF;i++) if($i~/^steps=/){split($i,a,"=");print a[2]}}')
  echo "$a,$N,$runs,$pu,$live,$px,$ins,$upd,$rng,$cur,$st" >> "$CSV"
}

for N in 50000 200000; do
  for arm in 0 1 2 3 4 5 6; do
    run_one "$N" "$arm"
  done
done
echo "--- csv ---" | tee -a "$LOG"
cat "$CSV" | tee -a "$LOG"
