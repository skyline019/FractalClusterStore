#!/bin/bash
set -euo pipefail
cd /mnt/e/FCS/embdb/x
mkdir -p backup/r18
LOG=backup/r18/layout_ab.log
: > "$LOG"
for prot in 0 1; do
  for pct in 0 150; do
    for rep in 1 2; do
      echo "=== protect=$prot pct=$pct rep=$rep ===" | tee -a "$LOG"
      ./build/layout 200000 500 32 1 1 0 "$pct" "$prot" 2>&1 | tee -a "$LOG" | grep -E '^== after update|^== merge' || true
    done
  done
done
echo DONE | tee -a "$LOG"
