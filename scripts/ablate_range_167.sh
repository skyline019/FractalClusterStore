#!/usr/bin/env bash
# 坑 167：已 Negate（fences+seek load 配对 1.29×）。历史入口 only — 不再编跑已删 API。
# 见 backup/range-167/VERDICT_167.md · backup/work-dist/ablate_r167_paired.cpp · CONVERGE_OPS 坑 171
set -euo pipefail
cd "$(dirname "$0")/.."
echo "PIT 167 Negate: fence+seek load rolled back; API removed under converge (pit 171)."
echo "Evidence: backup/work-dist/ablate_r167_paired.cpp (historical note binary)."
echo "ABLATED_167_NEGATE_DOCUMENTED"
exit 0
