#!/usr/bin/env bash
# Soft sync 50k：off/both 计数 + insert/update（归因写税是否开火）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
mkdir -p backup/soft-a1
for arm in off both fit prom; do
  case $arm in
    off) sf=0; sp=0 ;;
    both) sf=1; sp=1 ;;
    fit) sf=1; sp=0 ;;
    prom) sf=0; sp=1 ;;
  esac
  echo "== Soft sync arm=$arm =="
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm
  log=backup/soft-a1/sync_attr_${arm}.log
  ./build/bench4 50000 500 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 0 0 "$sf" "$sp" \
    >"$log" 2>&1
  grep -aE '^# FCS|^# fcs p2|^fcs insert|^fcs update|^fcs get' "$log" || true
done
