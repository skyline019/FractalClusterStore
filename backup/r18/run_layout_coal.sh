#!/bin/bash
# 有界并洞 B A/B：frag 开着，对照 coal 开/关；确定性 200k + 800k
set -euo pipefail
cd /mnt/e/FCS/embdb/x
LOG=backup/r18/layout_coal.log
: > "$LOG"
g++ -std=c++17 -O2 -pthread -Iinclude bench/probe/layout.cpp -o build/layout
# argv: N B VSZ mode quiet compact rpct protect budget_ms units frag coal
for coal in 0 1; do
  for rep in 1 2; do
    echo "=== coal=$coal frag=1 det200k rep=$rep ===" | tee -a "$LOG"
    ./build/layout 200000 500 32 1 1 0 0 0 0 8 1 "$coal" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
  done
  echo "=== coal=$coal frag=1 800k ===" | tee -a "$LOG"
  ./build/layout 800000 500 32 1 1 0 0 0 -1 0 1 "$coal" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
done
echo DONE | tee -a "$LOG"
