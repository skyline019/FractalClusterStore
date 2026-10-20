#!/bin/bash
# 确定性：budget=0 + units=8（同 g17 t_hole_fit）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
LOG=backup/r18/layout_det.log
: > "$LOG"
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout
for prot in 0 1; do
  for rep in 1 2; do
    echo "=== protect=$prot det budget0 units8 rep=$rep ===" | tee -a "$LOG"
    ./build/layout 200000 500 32 1 1 0 0 "$prot" 0 8 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
  done
done
# 800k 各 1 次（默认预算 + 护洞开/关）
for prot in 0 1; do
  echo "=== protect=$prot 800k pct0 ===" | tee -a "$LOG"
  ./build/layout 800000 500 32 1 1 0 0 "$prot" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
done
echo DONE | tee -a "$LOG"
