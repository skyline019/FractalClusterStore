#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# script lives in embdb/x/backup/work-dist → ../.. = embdb/x
cd "$ROOT"
test -f include/fcs.h
cp include/fcs.h /tmp/fcs.h.bak162
sed -i 's/bool flush_tip_ = false;/bool flush_tip_ = true;  \/\/ TEMP g16 tip-on/' include/fcs.h
make -j"$(nproc)" build/g16 build/f1 2>&1 | tail -5
echo "=== g16 tip=on ==="
set +e
build/g16
ec=$?
set -e
echo "g16_ec=$ec"
echo "=== f1 tip=on ==="
build/f1 | tail -8
mv /tmp/fcs.h.bak162 include/fcs.h
make -j"$(nproc)" build/g16 >/dev/null
echo "restored tip=false default"
exit "$ec"
