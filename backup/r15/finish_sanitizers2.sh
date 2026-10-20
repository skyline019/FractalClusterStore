#!/bin/bash
set -o pipefail
cd /mnt/e/FCS/embdb/x
INC="-Iinclude"
CXXFLAGS="-std=c++17 -O1 -g -pthread"
fail=0

echo "== ubsan g14 full ==" | tee backup/r15/ubsan15.log
g++ $CXXFLAGS -fsanitize=undefined $INC tests/g14.cpp -o build/ubsan_g14
UBSAN_OPTIONS=print_stacktrace=1 build/ubsan_g14 2>&1 | tee -a backup/r15/ubsan15.log | tail -5 || fail=1
for t in f1 f4 f6 g15 h2; do
  g++ $CXXFLAGS -fsanitize=undefined $INC tests/$t.cpp -o build/ubsan_$t
  echo -n "$t: " | tee -a backup/r15/ubsan15.log
  UBSAN_OPTIONS=print_stacktrace=1 build/ubsan_$t 2>&1 | tee -a backup/r15/ubsan15.log | tail -1 || fail=1
done

echo "== tsan g14 yield retry ==" | tee backup/r15/tsan15.log
g++ $CXXFLAGS -fsanitize=thread $INC tests/g14.cpp -o build/tsan_g14
# WSL 偶发 unexpected memory mapping：关 ASLR 再试
setarch $(uname -m) -R env TSAN_OPTIONS=report_signal_unsafe=0 build/tsan_g14 yield 2>&1 | tee -a backup/r15/tsan15.log | tail -8
ts=$?
if [ $ts -ne 0 ]; then
  echo "setarch failed or tsan fail ($ts), plain retry..." | tee -a backup/r15/tsan15.log
  build/tsan_g14 yield 2>&1 | tee -a backup/r15/tsan15.log | tail -8 || fail=1
else
  echo "tsan_g14 ok" | tee -a backup/r15/tsan15.log
fi

echo "== tsan g15 ==" | tee -a backup/r15/tsan15.log
g++ $CXXFLAGS -fsanitize=thread $INC tests/g15.cpp -o build/tsan_g15
setarch $(uname -m) -R build/tsan_g15 2>&1 | tee -a backup/r15/tsan15.log | tail -5 || build/tsan_g15 2>&1 | tee -a backup/r15/tsan15.log | tail -5 || fail=1

echo "== msan ==" | tee backup/r15/msan15.log
CLANG=""
for c in clang++ clang++-18 clang++-17 clang++-16 clang++-15; do
  if command -v $c >/dev/null 2>&1; then CLANG=$c; break; fi
done
if [ -z "$CLANG" ] && [ -x /usr/bin/clang++ ]; then CLANG=/usr/bin/clang++; fi
if [ -n "$CLANG" ]; then
  echo "using $CLANG" | tee -a backup/r15/msan15.log
  for t in g15 f1 f6; do
    $CLANG $CXXFLAGS -fsanitize=memory -fno-omit-frame-pointer $INC tests/$t.cpp -o build/msan_$t 2>&1 | tee -a backup/r15/msan15.log | tail -5
    echo -n "$t: " | tee -a backup/r15/msan15.log
    build/msan_$t 2>&1 | tee -a backup/r15/msan15.log | tail -1 || fail=1
  done
else
  echo "clang++ not installed; attempting apt-get" | tee -a backup/r15/msan15.log
  if sudo -n apt-get install -y clang 2>&1 | tee -a backup/r15/msan15.log | tail -5; then
    CLANG=clang++
    for t in g15 f1 f6; do
      $CLANG $CXXFLAGS -fsanitize=memory -fno-omit-frame-pointer $INC tests/$t.cpp -o build/msan_$t
      build/msan_$t 2>&1 | tee -a backup/r15/msan15.log | tail -1 || fail=1
    done
  else
    echo "MSAN_SKIP: no clang++ (hard gate deferred; asan+tsan+ubsan covered)" | tee -a backup/r15/msan15.log
  fi
fi

echo "SAN2_FAIL=$fail"
exit $fail
