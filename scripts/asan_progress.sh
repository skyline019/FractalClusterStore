#!/usr/bin/env bash
# 带进度的 make asan 等价物（避免 | tail -1 把整段 stdout 堵到结束才刷）
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=build
INC=-Iinclude
CXX="${CXX:-g++}"
TSAN_WRAP=$(command -v setarch >/dev/null 2>&1 && echo "setarch $(uname -m) -R" || echo "")
mkdir -p "$BIN"

run_asan() {
  local name="$1"; shift
  echo "== asan $name =="
  "$BIN/asan_$name" "$@" | tee /tmp/asan_"$name".out | tail -3
}

echo "# asan_progress start $(date -Iseconds)"

$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/f1.cpp -o $BIN/asan_f1
ASAN_OPTIONS=detect_leaks=1 run_asan f1
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/f4.cpp -o $BIN/asan_f4
ASAN_OPTIONS=detect_leaks=1 run_asan f4
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/q1.cpp -o $BIN/asan_q1
ASAN_OPTIONS=detect_leaks=1 run_asan q1
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/f5.cpp -o $BIN/asan_f5
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/f6.cpp -o $BIN/asan_f6
ASAN_OPTIONS=detect_leaks=1 run_asan f5
ASAN_OPTIONS=detect_leaks=1 run_asan f6
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/t3.cpp -o $BIN/asan_t3
run_asan t3
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/t2.cpp -o $BIN/asan_t2
echo "== asan t2 =="; ASAN_OPTIONS=detect_leaks=1 $BIN/asan_t2 2 | tee /tmp/asan_t2.out | tail -3
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/m1.cpp -o $BIN/asan_m1
ASAN_OPTIONS=detect_leaks=1 run_asan m1

for t in v1 g1 g2 g3 g4 g5 g6 g7 g8 g9 g10 g11 g12 g13 g14 g15 g16 g17 g18 h1 h2 h3 s3 s4 c1 c2 c3 c4 c5 c6; do
  echo "== asan $t compile =="
  $CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/$t.cpp -o $BIN/asan_$t
  echo "== asan $t run =="
  ASAN_OPTIONS=detect_leaks=1 $BIN/asan_$t | tee /tmp/asan_$t.out | tail -3
done

$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/w1.cpp -o $BIN/asan_w1
ASAN_OPTIONS=detect_leaks=1 run_asan w1

echo "== tsan w1 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/w1.cpp -o $BIN/tsan_w1
$TSAN_WRAP $BIN/tsan_w1 | tee /tmp/tsan_w1.out | tail -3

echo "== tsan g7 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g7.cpp -o $BIN/tsan_g7
$TSAN_WRAP $BIN/tsan_g7 2>&1 | tee /tmp/tsan_g7.out | tail -3

echo "== tsan g11 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g11.cpp -o $BIN/tsan_g11
$TSAN_WRAP $BIN/tsan_g11 2>&1 | tee /tmp/tsan_g11.out | tail -3

echo "== tsan g13 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g13.cpp -o $BIN/tsan_g13
$TSAN_WRAP $BIN/tsan_g13 mode3 2>&1 | tee /tmp/tsan_g13.out | tail -3

echo "== tsan g17 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g17.cpp -o $BIN/tsan_g17
$TSAN_WRAP $BIN/tsan_g17 60 >$BIN/tsan_g17.log 2>&1 || { echo '--- tsan_g17 log ---'; cat $BIN/tsan_g17.log; exit 1; }
tail -3 $BIN/tsan_g17.log

echo "== tsan g14 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=thread $INC tests/g14.cpp -o $BIN/tsan_g14
$TSAN_WRAP $BIN/tsan_g14 yield 2>&1 | tee /tmp/tsan_g14.out | tail -3

echo "== tsan g7 stress =="
$TSAN_WRAP $BIN/tsan_g7 3 3000 4 2>&1 | tee /tmp/tsan_g7s.out | tail -3
G7_EXCL=1 $TSAN_WRAP $BIN/tsan_g7 1 2000 2>&1 | tee /tmp/tsan_g7e.out | tail -3

echo "== asan r2 s2 =="
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/r2.cpp -o $BIN/asan_r2
ASAN_OPTIONS=detect_leaks=1 run_asan r2
$CXX -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $INC tests/s2.cpp -o $BIN/asan_s2
ASAN_OPTIONS=detect_leaks=1 run_asan s2

echo "ASAN_PROGRESS_OK $(date -Iseconds)"
