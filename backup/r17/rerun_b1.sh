#!/bin/bash
set -e
cd /mnt/e/FCS/embdb/x
for i in 1 2 3; do
  rm -f build/big2.db
  echo "=== run $i ==="
  ./build/b1 2>&1 | tee "backup/r17/b1_rerun_$i.log" | grep -E 'A:|B:|PASS|FAIL|verify=|errors='
done
echo ALL_DONE
