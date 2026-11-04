#!/usr/bin/env bash
# 坑 146：Soft range 根源诊断（路径 C）— 结构 + merge_diag + 实验臂
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt/root build
OUT=backup/range-opt/root
LOG=$OUT/range_root.log
CSV=$OUT/range_root.csv
: > "$LOG"
echo "arm,N,runs,cells,insert,update,range,cursors,lvl_hist,fam_hist,skip_need_t,skip_not_trigger,did_reclaim,did_cheap,did_cliff,did_d,did_merge" > "$CSV"

g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_root.cpp -o build/range_root

run_one() {
  local N=$1 arm=$2
  echo "== N=$N arm=$arm ==" | tee -a "$LOG"
  local block
  block=$(./build/range_root "$N" 500 2000 100 "$arm")
  echo "$block" | tee -a "$LOG"
  # parse first line of second run for csv stability (rep2)
  local a runs cells ins upd rng cur
  a=$(echo "$block" | awk '/^arm=/{print $1}' | head -1 | sed 's/arm=//')
  runs=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^runs=/){split($i,a,"="); print a[2]}}' | head -1)
  cells=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^cells=/){split($i,a,"="); print a[2]}}' | head -1)
  ins=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^insert=/){split($i,a,"="); print a[2]}}' | head -1)
  upd=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^update=/){split($i,a,"="); print a[2]}}' | head -1)
  rng=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^range=/){split($i,a,"="); print a[2]}}' | head -1)
  cur=$(echo "$block" | awk '/^arm=/{for(i=1;i<=NF;i++) if($i~/^cursors=/){split($i,a,"="); print a[2]}}' | head -1)
  local lh fh
  lh=$(echo "$block" | awk '/^lvl_hist=/{print}' | head -1 | sed 's/ lvl_hist=/;/' | sed 's/fam_hist=/ /' | awk '{print $1}' | sed 's/lvl_hist=//')
  fh=$(echo "$block" | awk '/fam_hist=/{print}' | head -1 | sed 's/.*fam_hist=//')
  local snt sntg dr dc dcl dd dm
  snt=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/skip_need_t=/){split($i,a,"="); print a[2]}}' | head -1)
  sntg=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/skip_not_trigger=/){split($i,a,"="); print a[2]}}' | head -1)
  dr=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/did_reclaim=/){split($i,a,"="); print a[2]}}' | head -1)
  dc=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/did_cheap=/){split($i,a,"="); print a[2]}}' | head -1)
  dcl=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/did_cliff=/){split($i,a,"="); print a[2]}}' | head -1)
  dd=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/^did_d=/){split($i,a,"="); print a[2]}}' | head -1)
  dm=$(echo "$block" | awk '/^md /{for(i=1;i<=NF;i++) if($i~/did_merge=/){split($i,a,"="); print a[2]}}' | head -1)
  echo "$a,$N,$runs,$cells,$ins,$upd,$rng,$cur,$lh,$fh,$snt,$sntg,$dr,$dc,$dcl,$dd,$dm" >> "$CSV"
}

# Soft 50k：全臂；Soft 200k：baseline + polish + 若 cliff/cheap 有戏再看（全跑省事）
for N in 50000 200000; do
  for arm in 0 1 2 3 4; do
    run_one "$N" "$arm"
  done
done

echo "--- csv ---" | tee -a "$LOG"
cat "$CSV" | tee -a "$LOG"
echo "DONE -> $OUT"
