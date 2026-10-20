#!/bin/bash
set -euo pipefail
X=/mnt/e/FCS/embdb/x
echo "include headers:"
find "$X/include" -name '*.h' | xargs wc -l | tail -1
echo "tests cpp:"
find "$X/tests" -maxdepth 1 -name '*.cpp' | xargs wc -l | tail -1
echo "bench cpp:"
find "$X/bench" -name '*.cpp' | xargs wc -l | tail -1
echo "web front js/jsx/css/html:"
find "$X/web" -type f \( -name '*.js' -o -name '*.jsx' -o -name '*.css' -o -name '*.html' \) | xargs wc -l | tail -1
echo "docs md:"
find "$X/docs" -name '*.md' | xargs wc -l | tail -1
echo "README:"
wc -l "$X/README.md"
echo "Makefile:"
wc -l "$X/Makefile"
echo "C/C++ total exclude backup+build:"
find "$X" \( -path "$X/backup" -o -path "$X/build" \) -prune -o -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' \) -print | xargs wc -l | tail -1
echo "all text-ish exclude backup+build h/cpp/md/sh/Makefile:"
find "$X" \( -path "$X/backup" -o -path "$X/build" \) -prune -o -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.md' -o -name '*.sh' -o -name 'Makefile' -o -name '*.js' -o -name '*.jsx' -o -name '*.css' -o -name '*.html' \) -print | xargs wc -l | tail -1
echo
echo "embdb/new C/C++:"
find /mnt/e/FCS/embdb/new \( -path '*/backup' -o -path '*/backup/*' -o -path '*/build' -o -path '*/build/*' \) -prune -o -type f \( -name '*.h' -o -name '*.cpp' \) -print | xargs wc -l | tail -1
echo "embdb/src+include v1:"
wc -l /mnt/e/FCS/embdb/src/engine.cpp /mnt/e/FCS/embdb/include/*.h | tail -1
echo "FCS disk:"
du -sh /mnt/e/FCS /mnt/e/FCS/embdb /mnt/e/FCS/embdb/x /mnt/e/FCS/embdb/x/backup /mnt/e/FCS/embdb/new 2>/dev/null
