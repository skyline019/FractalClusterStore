#!/usr/bin/env bash
# 全量 make test + Soft peers（顺序执行，结果进本目录日志）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
OUT=backup/peers-soft
mkdir -p "$OUT"

echo "======== make test $(date -Iseconds) ========" | tee "$OUT/make_test.log"
make test 2>&1 | tee -a "$OUT/make_test.log"
echo "MAKE_TEST_OK" | tee -a "$OUT/make_test.log"

bash backup/peers-soft/run_peers_soft.sh 2>&1 | tee "$OUT/peers_soft_run.log"
echo ALL_DONE
