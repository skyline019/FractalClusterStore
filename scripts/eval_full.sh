#!/usr/bin/env bash
# 完整全量评估：正确性门禁 + Soft Post peers×5 + Soft A1 空间峰
# 输出 backup/eval-full/<date>/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
STAMP="${STAMP:-$(date +%Y-%m-%d)}"
OUT="${OUT:-backup/eval-full/$STAMP}"
mkdir -p "$OUT"
exec > >(tee "$OUT/run.log") 2>&1

echo "# FCS full eval $STAMP"
echo "# host: $(uname -a | tr '\n' ' ')"
echo "# date: $(date -Iseconds)"
echo "# fcs.h lines: $(wc -l < include/fcs.h)"
echo "# HEAD: $(git rev-parse --short HEAD 2>/dev/null || echo n/a)"
echo

pass() { echo "PASS $1"; echo "$1" >> "$OUT/PASS.txt"; }
fail() { echo "FAIL $1"; echo "$1" >> "$OUT/FAIL.txt"; exit 1; }

: > "$OUT/PASS.txt"
: > "$OUT/FAIL.txt"

echo "== make bins =="
make bins
pass bins

echo "== make test =="
if make test >"$OUT/make_test.log" 2>&1; then
  tail -30 "$OUT/make_test.log"
  pass make_test
else
  tail -80 "$OUT/make_test.log"
  fail make_test
fi

echo "== make soak-smoke =="
if make soak-smoke >"$OUT/soak_smoke.log" 2>&1; then
  tail -20 "$OUT/soak_smoke.log"
  pass soak_smoke
else
  tail -40 "$OUT/soak_smoke.log"
  fail soak_smoke
fi

echo "== asan (progress script; ~30–60 min, not hung if compiling/running) =="
# 坑 168：勿只用 make asan|tee——内部 |tail -1 会长时间无输出，像卡死
if bash scripts/asan_progress.sh >"$OUT/asan.log" 2>&1; then
  tail -40 "$OUT/asan.log"
  pass asan
else
  if grep -q "unexpected memory mapping\|SUMMARY: ThreadSanitizer" "$OUT/asan.log"; then
    echo "NOTE: TSan mapping (pit 122); Makefile TSAN_WRAP should already use setarch -R"
  fi
  tail -80 "$OUT/asan.log"
  fail asan
fi

echo "== peers_full_median R=5 =="
R=5 OUT="$OUT/peers" bash scripts/peers_full_median.sh
cp -f "$OUT/peers/PEERS_FULL_MED.md" "$OUT/PEERS_FULL_MED.md" 2>/dev/null || true
pass peers_median

echo "== soft-a1 (space peak) =="
if make soft-a1 >"$OUT/soft_a1.log" 2>&1; then
  tail -40 "$OUT/soft_a1.log"
  pass soft_a1
else
  tail -80 "$OUT/soft_a1.log"
  fail soft_a1
fi

echo
echo "==== EVAL_FULL SUMMARY ===="
echo "PASS: $(tr '\n' ' ' < "$OUT/PASS.txt")"
echo "OUT=$OUT"
echo "EVAL_FULL_OK"
