#!/usr/bin/env bash
# Local reproduce of Strict CI compile gate (same flags as .github/workflows/ci.yml).
set -euo pipefail
cd "$(dirname "$0")/.."
export CI_CXXFLAGS="-std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=format -Wno-error=narrowing -Wno-unused-parameter -pthread"
rm -rf build
mkdir -p build
make -j"$(nproc)" CXXFLAGS="${CI_CXXFLAGS}" bins
echo "ci-compile OK"
