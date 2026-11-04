#!/usr/bin/env bash
# Soft sync 工作节奏（坑 148）
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/sync_work_trace.cpp -o build/sync_work_trace

for N in 50000 200000; do
  OUT=backup/sync-work/n${N}
  mkdir -p "$OUT"
  echo "==== Soft sync N=$N ===="
  ./build/sync_work_trace "$N" 500 "$OUT"
  python3 scripts/analyze_sync_work.py "$OUT"
done
echo "DONE backup/sync-work/"
