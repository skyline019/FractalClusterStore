#!/bin/bash
set -euo pipefail
ROOT=/mnt/e/FCS
X=$ROOT/embdb/x
cd "$ROOT"

echo "======== FCS workspace overview ========"
du -sh "$ROOT" "$ROOT/embdb" "$X" 2>/dev/null || true
echo

echo "======== embdb/x: source LOC exclude backup+build ========"
find "$X" \( -path "$X/backup" -o -path "$X/build" \) -prune -o \
  -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.c' -o -name '*.cc' \) -print0 \
  | xargs -0 wc -l | tee /tmp/fcs_loc_all.txt | tail -5
echo

echo "======== embdb/x: headers ========"
find "$X/include" -type f -name '*.h' -print0 | xargs -0 wc -l | sort -n
echo

echo "======== embdb/x: tests/*.cpp ========"
find "$X/tests" -maxdepth 1 -name '*.cpp' -print0 | xargs -0 wc -l | sort -n | tail -20
echo -n "test cpp files: "; find "$X/tests" -maxdepth 1 -name '*.cpp' | wc -l
echo -n "test cpp total: "; find "$X/tests" -maxdepth 1 -name '*.cpp' -print0 | xargs -0 wc -l | tail -1
echo

echo "======== embdb/x: bench ========"
find "$X/bench" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.sh' \) -print0 2>/dev/null \
  | xargs -0 wc -l 2>/dev/null | tail -3
echo

echo "======== embdb/x: docs md ========"
find "$X/docs" -name '*.md' -print0 2>/dev/null | xargs -0 wc -l 2>/dev/null | sort -n | tail -15
echo -n "docs total: "; find "$X/docs" -name '*.md' -print0 | xargs -0 wc -l | tail -1
echo

echo "======== embdb/x: Makefile + sh ========"
wc -l "$X/Makefile" 2>/dev/null
find "$X" \( -path "$X/backup" -o -path "$X/build" \) -prune -o -type f -name '*.sh' -print0 \
  | xargs -0 wc -l 2>/dev/null | tail -3
echo

echo "======== embdb/x: by extension file count ========"
find "$X" \( -path "$X/backup" -o -path "$X/build" \) -prune -o -type f -print \
  | sed 's/.*\.//' | sort | uniq -c | sort -rn | head -20
echo

echo "======== embdb whole tree source exclude backup+build ========"
find "$ROOT/embdb" \( -path '*/backup' -o -path '*/backup/*' -o -path '*/build' -o -path '*/build/*' \) -prune -o \
  -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' \) -print0 \
  | xargs -0 wc -l 2>/dev/null | tail -3
echo

echo "======== FCS root top-level dirs ========"
for d in "$ROOT"/*; do
  [ -d "$d" ] || continue
  n=$(find "$d" \( -path '*/backup' -o -path '*/backup/*' -o -path '*/build' -o -path '*/build/*' -o -path '*/.git' -o -path '*/.git/*' \) -prune -o \
    -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.py' -o -name '*.rs' -o -name '*.go' -o -name '*.ts' -o -name '*.js' \) -print0 2>/dev/null \
    | xargs -0 wc -l 2>/dev/null | tail -1 | awk '{print $1}')
  echo "  $(basename "$d"): ${n:-0} LOC (code-ish)"
done

if command -v cloc >/dev/null 2>&1; then
  echo
  echo "======== cloc embdb/x ========"
  cloc "$X" --exclude-dir=backup,build,golden,.git --quiet
fi
