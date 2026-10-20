#!/bin/bash
cd /mnt/e/FCS/embdb/x
echo "=== ROOT ==="
ls -la
echo "=== docs ==="
ls -la docs
echo "=== tools ==="
ls -la tools
echo "=== web ==="
ls -la web
echo "=== bench ==="
ls -la bench
echo "=== build count ==="
ls build 2>/dev/null | wc -l
echo "=== backup top ==="
ls -la backup
echo "=== tests golden ==="
ls -la tests/golden 2>/dev/null | head -20
echo "=== root loose files ==="
find . -maxdepth 1 -type f
echo "=== logs outside backup ==="
find . -name '*.log' -not -path './backup/*' 2>/dev/null
echo "=== imgs outside golden/backup ==="
find . -name '*.img' -not -path './backup/*' -not -path './tests/golden/*' 2>/dev/null
echo "=== tmp/odd ==="
find . -maxdepth 3 \( -name '*.tmp' -o -name '*~' -o -name '*.bak' -o -name '*.orig' -o -name '*.o' \) -not -path './backup/*' 2>/dev/null
