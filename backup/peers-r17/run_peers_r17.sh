#!/usr/bin/env bash
# R17 主树：重跑 FCS↔SQLite（bench4）与 FCS↔LMDB（bench7）基线，归档到 backup/peers-r17/
set -euo pipefail
cd /mnt/e/FCS/embdb/x
OUT=backup/peers-r17
mkdir -p "$OUT" build
HOST=$(uname -a | tr '\n' ' ')
echo "# host: $HOST" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# fcs.h: $(wc -l < include/fcs.h) lines; FCS_R17=$(grep -c 'FCS_R17 1' include/fcs.h || true)" | tee -a "$OUT/META.txt"

# Prefer native FS paths (already /home/user/.*)
rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm
rm -f /home/user/.fcs_peers.db
rm -rf /home/user/.lmdb_peers

echo "== build =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench7.cpp -o build/bench7 -llmdb
echo BUILD_OK

# --- SQLite (bench4)：与 wsl-20261020 同参 ---
echo "== bench4 sync =="
./build/bench4 50000 500 32 50000 2000 100 | tee "$OUT/bench4_sync.log"
echo "== bench4 async+group+cons+shr =="
./build/bench4 50000 500 32 50000 2000 100 0 1 1 8 0 1 1 | tee "$OUT/bench4_async_cons.log"
echo "== bench4 200k sync =="
./build/bench4 200000 2000 100 100000 2000 100 | tee "$OUT/bench4_n200k.log"

# --- LMDB (bench7) ---
echo "== bench7 async+group+cons+shr =="
./build/bench7 50000 500 32 50000 2000 100 1 1 1 1 | tee "$OUT/bench7_async_cons.log"
echo "== bench7 sync =="
./build/bench7 50000 500 32 50000 2000 100 0 0 0 0 | tee "$OUT/bench7_sync.log"

echo ALL_PEERS_OK
