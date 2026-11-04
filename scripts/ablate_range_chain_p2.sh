#!/usr/bin/env bash
# pit 145: family-chain under Soft A1-scale (P2 slices likely)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt build
OUT=backup/range-opt/chain_p2.log
: > "$OUT"
cat > /tmp/range_p2.cpp <<'EOF'
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27;
    x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x;
}
static double now_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 200000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    const char* P = "/tmp/range_p2.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 60e6) s.commit(); s.commit(); }
    u64 co0 = s.cursors_opened();
    double t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double us = (now_us() - t0) / (double)S;
    std::printf("runs=%u range=%.3f cursors=%.2f fits=%llu prom=%llu rows=%llu\n",
                s.run_count(), us, (double)(s.cursors_opened() - co0) / S,
                (unsigned long long)s.part_starve_fits(),
                (unsigned long long)s.starve_promotes(),
                (unsigned long long)got);
    std::remove(P);
    return 0;
}
EOF
for arm in "chain=1:1" "chain=0:0"; do
  tag=${arm%%:*}; c=${arm##*:}
  g++ -std=c++17 -O2 -g -pthread -Iinclude -DFCS_RANGE_FAMILY_CHAIN=$c \
      /tmp/range_p2.cpp -o build/range_p2_$tag
  echo "== $tag ==" | tee -a "$OUT"
  for rep in 1 2 3; do
    setarch "$(uname -m)" -R ./build/range_p2_$tag 200000 500 2000 100 2>/dev/null \
      || ./build/range_p2_$tag 200000 500 2000 100
  done | tee -a "$OUT"
done
