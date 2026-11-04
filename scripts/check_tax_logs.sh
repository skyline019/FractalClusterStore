#!/usr/bin/env bash
set -euo pipefail
cd /mnt/e/FCS/embdb/x
for f in backup/soft-a1/tax_r1_off.log backup/soft-a1/tax_r1_fit.log backup/soft-a1/tax_r1_both.log; do
  echo "== $f =="
  grep -E 'p2:|starve_|Soft A1|bf_mode|starve_fit' "$f" | head -5
done
