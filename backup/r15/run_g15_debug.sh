#!/bin/bash
cd /mnt/e/FCS/embdb/x
mkdir -p build
echo "=== compile asan ==="
g++ -std=c++17 -O1 -g -pthread -fsanitize=address -fno-omit-frame-pointer \
  -Iinclude tests/g15.cpp -o build/g15_asan 2>&1 | head -40
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
for t in fmt trim_unit trim_replay snap bad_trim job_peak; do
  echo "=== $t ==="
  build/g15_asan "$t" 2>&1
  echo "exit:$?"
done
echo "=== full ==="
build/g15_asan 2>&1
echo "exit:$?"
