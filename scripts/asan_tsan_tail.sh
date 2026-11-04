#!/usr/bin/env bash
# Resume make asan from the TSan section (ASan loop already verified).
set -euo pipefail
cd "$(dirname "$0")/.."
WRAP=(setarch "$(uname -m)" -R)
CXX="${CXX:-g++}"
INC=-Iinclude
BIN=build

run() { echo "+ $*"; "${WRAP[@]}" "$@" 2>&1 | tee /tmp/asan_tail.last | tail -3; }

$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/w1.cpp -o $BIN/tsan_w1
run $BIN/tsan_w1
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g7.cpp -o $BIN/tsan_g7
run $BIN/tsan_g7
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g11.cpp -o $BIN/tsan_g11
run $BIN/tsan_g11
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g13.cpp -o $BIN/tsan_g13
run $BIN/tsan_g13 mode3
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g17.cpp -o $BIN/tsan_g17
run $BIN/tsan_g17 60
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g14.cpp -o $BIN/tsan_g14
run $BIN/tsan_g14 yield
run $BIN/tsan_g7 3 3000 4
G7_EXCL=1 run $BIN/tsan_g7 1 2000
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/r2.cpp -o $BIN/asan_r2
ASAN_OPTIONS=detect_leaks=1 $BIN/asan_r2 | tee /tmp/asan_tail.last | tail -1
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/s2.cpp -o $BIN/asan_s2
ASAN_OPTIONS=detect_leaks=1 $BIN/asan_s2 | tee /tmp/asan_tail.last | tail -1
echo ASAN_TSAN_TAIL_OK=1
