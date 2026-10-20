#!/bin/bash
# zip 展开后文件 mtime 常为 1980，make 会跳过重编 → 混跑旧二进制。
set -euo pipefail
cd /mnt/e/FCS/embdb/x
NOW=$(date +%Y%m%d%H%M.%S)
find include tests bench -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.img' -o -name '*.kv' \) -exec touch -t "$NOW" {} +
# 清掉测试二进制（保留 *.db 等大文件）
find build -maxdepth 1 -type f -executable -delete 2>/dev/null || true
rm -f build/big2.db
make -j2 -s test 2>&1 | tee backup/r17/make_test_promote2.log
echo MAKE_TEST_EXIT:$?
