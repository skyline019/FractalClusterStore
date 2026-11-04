#!/usr/bin/env bash
# 坑 147：路径 B 可选项消融（Soft 默认不升）
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt/root build
OUT=backup/range-opt/root
LOG=$OUT/b_ablate.log
CSV=$OUT/b_ablate.csv
: > "$LOG"
echo "arm,N,runs,used,live,peak_x,insert,update,range,cursors" > "$CSV"
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_b_ablate.cpp -o build/range_b_ablate

run_one() {
  local N=$1 arm=$2
  echo "== N=$N arm=$arm ==" | tee -a "$LOG"
  local block
  block=$(./build/range_b_ablate "$N" 500 2000 100 "$arm")
  echo "$block" | tee -a "$LOG"
  local a runs used live px ins upd rng cur
  a=$(echo "$block" | awk '/^arm=/{split($1,a,"="); print a[2]}' | head -1)
  runs=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^runs=/){split($i,a,"=");print a[2]}}' | head -1)
  used=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^used=/){split($i,a,"=");print a[2]}}' | head -1)
  live=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^live=/){split($i,a,"=");print a[2]}}' | head -1)
  px=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}' | head -1)
  ins=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^insert=/){split($i,a,"=");print a[2]}}' | head -1)
  upd=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^update=/){split($i,a,"=");print a[2]}}' | head -1)
  rng=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^range=/){split($i,a,"=");print a[2]}}' | head -1)
  cur=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^cursors=/){split($i,a,"=");print a[2]}}' | head -1)
  echo "$a,$N,$runs,$used,$live,$px,$ins,$upd,$rng,$cur" >> "$CSV"
}

for N in 50000 200000; do
  for arm in 0 1 2 3; do
    run_one "$N" "$arm"
  done
done
echo "--- csv ---" | tee -a "$LOG"
cat "$CSV" | tee -a "$LOG"
