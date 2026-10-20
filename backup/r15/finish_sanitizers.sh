#!/bin/bash
set -o pipefail
cd /mnt/e/FCS/embdb/x
mkdir -p build
INC="-Iinclude"
CXXFLAGS="-std=c++17 -O1 -g -pthread"
LOG=backup/r15/asan15b.log
: > "$LOG"
run_asan() {
  local t=$1
  echo "== asan $t ==" | tee -a "$LOG"
  g++ $CXXFLAGS -fsanitize=address $INC tests/$t.cpp -o build/asan_$t 2>>"$LOG" || return 1
  ASAN_OPTIONS=detect_leaks=1 build/asan_$t 2>&1 | tee -a "$LOG" | tail -3
  return ${PIPESTATUS[0]}
}
fail=0
for t in g14 g15 h1 h2 h3 s3 s4 c1 w1 r2 s2; do
  if [ "$t" = h1 ]; then
    g++ $CXXFLAGS -fsanitize=address $INC tests/h1.cpp -o build/asan_h1
    ASAN_OPTIONS=detect_leaks=1 build/asan_h1 400 1 2>&1 | tee -a "$LOG" | tail -2
    ASAN_OPTIONS=detect_leaks=1 build/asan_h1 400 2 2>&1 | tee -a "$LOG" | tail -1
    continue
  fi
  run_asan "$t" || fail=1
done
echo "== tsan g15 ==" | tee -a "$LOG"
g++ $CXXFLAGS -fsanitize=thread $INC tests/g15.cpp -o build/tsan_g15
build/tsan_g15 2>&1 | tee -a "$LOG" | tail -3 || fail=1
echo "== tsan g14 yield ==" | tee -a "$LOG"
g++ $CXXFLAGS -fsanitize=thread $INC tests/g14.cpp -o build/tsan_g14
build/tsan_g14 yield 2>&1 | tee -a "$LOG" | tail -3 || fail=1

echo "== ubsan ==" | tee backup/r15/ubsan15.log
for t in f1 f4 f6 g14 g15 h2; do
  g++ $CXXFLAGS -fsanitize=undefined $INC tests/$t.cpp -o build/ubsan_$t
  echo -n "$t: " | tee -a backup/r15/ubsan15.log
  UBSAN_OPTIONS=print_stacktrace=1 build/ubsan_$t 2>&1 | tee -a backup/r15/ubsan15.log | tail -1
done

echo "== msan ==" | tee backup/r15/msan15.log
CLANG=$(command -v clang++ || true)
if [ -n "$CLANG" ]; then
  for t in g15 f1 f6; do
    $CLANG $CXXFLAGS -fsanitize=memory -fno-omit-frame-pointer $INC tests/$t.cpp -o build/msan_$t
    echo -n "$t: " | tee -a backup/r15/msan15.log
    build/msan_$t 2>&1 | tee -a backup/r15/msan15.log | tail -1 || fail=1
  done
else
  echo "clang++ missing, skip msan" | tee -a backup/r15/msan15.log
  fail=1
fi

echo "SAN_FAIL=$fail" | tee -a "$LOG"
exit $fail
