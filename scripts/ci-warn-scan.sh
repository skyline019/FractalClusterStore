#!/usr/bin/env bash
# Find remaining -Werror failures under Strict CI flags (compile each test TU).
set -euo pipefail
cd "$(dirname "$0")/.."
CI_CXXFLAGS="-std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=format -Wno-error=narrowing -Wno-unused-parameter -pthread"
fail=0
mkdir -p /tmp/ci-scan
for f in tests/*.cpp; do
  base=$(basename "$f" .cpp)
  if ! g++ ${CI_CXXFLAGS} -Iinclude -c "$f" -o "/tmp/ci-scan/${base}.o" 2>"/tmp/ci-scan/${base}.err"; then
    echo "==== FAIL $f"
    grep -E 'error:|fatal error:' "/tmp/ci-scan/${base}.err" || true
    fail=1
  fi
done
if [[ "$fail" -ne 0 ]]; then
  echo "ci-warn-scan: FAIL"
  exit 1
fi
echo "ci-warn-scan: OK"
