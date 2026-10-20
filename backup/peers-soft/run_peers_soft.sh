#!/usr/bin/env bash
# Soft peers：configure_deploy / TidyPolicy::Soft 下全量 FCS↔SQLite / FCS↔LMDB
# 参数与 peers-r17 对齐；bench argv 末位 soft=1
set -euo pipefail
cd /mnt/e/FCS/embdb/x
OUT=backup/peers-soft
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable"
  exit 1
fi

HOST=$(uname -a | tr '\n' ' ')
echo "# host: $HOST" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# mode: Soft / configure_deploy" | tee -a "$OUT/META.txt"
echo "# fcs.h: $(wc -l < include/fcs.h) lines" | tee -a "$OUT/META.txt"

rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
      /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm
rm -f /home/user/.fcs_peers.db
rm -rf /home/user/.lmdb_peers

echo "== build =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench7.cpp -o build/bench7 -llmdb
echo BUILD_OK

# argv: N B VSZ R S K kidx async gm lt am cons shr ithr kmode hwpct soft
echo "== bench4 Soft sync =="
./build/bench4 50000 500 32 50000 2000 100 0 0 0 0 0 0 0 1 0 0 1 \
  | tee "$OUT/bench4_soft_sync.log"

echo "== bench4 Soft async+group+cons+shr =="
./build/bench4 50000 500 32 50000 2000 100 0 1 1 8 0 1 1 1 0 0 1 \
  | tee "$OUT/bench4_soft_async_cons.log"

echo "== bench4 Soft 200k sync =="
./build/bench4 200000 2000 100 100000 2000 100 0 0 0 0 0 0 0 1 0 0 1 \
  | tee "$OUT/bench4_soft_n200k.log"

# argv: N B VSZ R S K async gm cons shr soft
echo "== bench7 Soft async+group+cons+shr =="
./build/bench7 50000 500 32 50000 2000 100 1 1 1 1 1 \
  | tee "$OUT/bench7_soft_async_cons.log"

echo "== bench7 Soft sync =="
./build/bench7 50000 500 32 50000 2000 100 0 0 0 0 1 \
  | tee "$OUT/bench7_soft_sync.log"

echo ALL_SOFT_PEERS_OK
