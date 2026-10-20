#!/bin/bash
# 碎片感知切片 A/B：确定性 200k + 800k 各对照
set -euo pipefail
cd /mnt/e/FCS/embdb/x
LOG=backup/r18/layout_frag.log
: > "$LOG"
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout
# argv: N B VSZ mode quiet compact rpct protect budget_ms units frag
for frag in 0 1; do
  for rep in 1 2; do
    echo "=== frag=$frag det200k rep=$rep ===" | tee -a "$LOG"
    ./build/layout 200000 500 32 1 1 0 0 0 0 8 "$frag" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
  done
  echo "=== frag=$frag 800k ===" | tee -a "$LOG"
  ./build/layout 800000 500 32 1 1 0 0 0 -1 0 "$frag" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
done
echo DONE | tee -a "$LOG"
