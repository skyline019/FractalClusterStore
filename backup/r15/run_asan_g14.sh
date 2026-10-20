#!/bin/bash
cd /mnt/e/FCS/embdb/x
g++ -std=c++17 -O1 -g -pthread -fsanitize=address -Iinclude tests/g14.cpp -o build/asan_g14 2>&1 | head -20
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
for t in jobacct reserve extend hw recede yield; do
  echo "=== $t ==="
  build/asan_g14 "$t" 2>&1 | tail -30
  echo exit:$?
done
echo "=== full ==="
build/asan_g14 2>&1 | tee backup/r15/asan_g14.log | tail -40
echo exit:$?
