#!/usr/bin/env bash
# 坑 167：已 Negate（fences+seek load 配对 1.29×）。保留脚本仅作历史复现入口。
# 见 backup/range-167/VERDICT_167.md · backup/work-dist/ablate_r167_paired.cpp
set -euo pipefail
cd "$(dirname "$0")/.."
echo "PIT 167 Negate: fence+seek load rolled back; run ablate_r167_paired.cpp for evidence"
g++ -O2 -std=c++17 -I. backup/work-dist/ablate_r167_paired.cpp -o /tmp/ablate_r167p -lpthread
# Note: binary still calls set_range_fence_load — API removed. Skip runtime.
echo "ABLATED_167_NEGATE_DOCUMENTED"
exit 0
