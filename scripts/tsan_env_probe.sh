#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
echo "kernel=$(uname -r)"
echo "mmap_rnd_bits=$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null || echo NA)"
echo "mmap_rnd_compat_bits=$(cat /proc/sys/vm/mmap_rnd_compat_bits 2>/dev/null || echo NA)"

run_one() {
  local tag=$1
  shift
  echo "=== $tag ==="
  set +e
  "$@" >/tmp/tsan_probe.out 2>/tmp/tsan_probe.err
  local rc=$?
  set -e
  echo "rc=$rc"
  if [[ -s /tmp/tsan_probe.err ]]; then head -5 /tmp/tsan_probe.err; fi
  tail -1 /tmp/tsan_probe.out || true
  return 0
}

test -x build/tsan_w1 || g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Iinclude tests/w1.cpp -o build/tsan_w1

run_one plain ./build/tsan_w1
run_one setarch_R setarch "$(uname -m)" -R ./build/tsan_w1

if sudo -n true 2>/dev/null; then
  sudo -n sysctl -w vm.mmap_rnd_bits=28 || true
elif [[ -w /proc/sys/vm/mmap_rnd_bits ]]; then
  echo 28 >/proc/sys/vm/mmap_rnd_bits || true
else
  echo "NO_SYSCTL_WRITE (need sudo for vm.mmap_rnd_bits=28)"
fi
echo "mmap_rnd_bits_now=$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null || echo NA)"
run_one after_sysctl ./build/tsan_w1
