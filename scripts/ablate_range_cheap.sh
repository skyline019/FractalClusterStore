#!/usr/bin/env bash
# Soft sync: cheap_pages ablation (pit 145)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt build
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_cheap_probe.cpp -o build/range_cheap_probe
OUT=backup/range-opt/cheap_ablate.csv
LOG=backup/range-opt/cheap_ablate.log
echo "cheap,runs,insert,update,get,range,cursors" > "$OUT"
: > "$LOG"
for c in 1024 512 256 128 64; do
  echo "== cheap=$c ==" | tee -a "$LOG"
  for rep in 1 2; do
    line=$(./build/range_cheap_probe "$c")
    echo "$line" | tee -a "$LOG"
    if [[ $rep -eq 2 ]]; then
      # cheap=.. runs=.. insert=.. update=.. get=.. range=.. cursors/scan=..
      printf '%s\n' "$line" | awk '{
        for (i=1;i<=NF;i++) {
          split($i, a, "=");
          if (a[1]=="cheap") c=a[2];
          if (a[1]=="runs") r=a[2];
          if (a[1]=="insert") ins=a[2];
          if (a[1]=="update") upd=a[2];
          if (a[1]=="get") g=a[2];
          if (a[1]=="range") rg=a[2];
          if (a[1]=="cursors/scan") cs=a[2];
        }
        printf "%s,%s,%s,%s,%s,%s,%s\n", c,r,ins,upd,g,rg,cs
      }' >> "$OUT"
    fi
  done
done
echo "--- csv ---"
cat "$OUT"
