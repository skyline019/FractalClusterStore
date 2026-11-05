#!/usr/bin/env bash
# Post-split gates for eng-debt header split.
set -euo pipefail
cd "$(dirname "$0")/.."
LOG=/tmp/split_make_test.log
echo "=== make test ==="
make test 2>&1 | tee "$LOG"
ec=${PIPESTATUS[0]}
echo "MAKE_TEST_EC=$ec"
exit "$ec"
