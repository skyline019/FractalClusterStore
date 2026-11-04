#!/usr/bin/env bash
# Soft range knives: measure + polish + lvl_t + cheap ablate (pit 145)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p backup/range-opt build
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/probe/range_tax.cpp -o build/range_tax

# also build a polish-after probe inline via env? use range_tax + separate polish probe
cat > /tmp/range_polish.cpp <<'EOF'
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
    int mode = argc > 5 ? atoi(argv[5]) : 0; // 0=soft 1=polish 2=lvl_t4 3=cliff2 4=chain_off_sim via p2off
    const char* P = "/tmp/range_polish.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * 56ull + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    if (mode == 2) s.set_lvl_t(4);
    if (mode == 3) s.set_lvl_cliff(2);
    if (mode == 4) { s.set_part_starve_fit(false); s.set_starve_promote(false); }
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
    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 30e6) s.commit(); s.commit(); }
    if (mode == 1) { s.polish(); }
    u64 co0 = s.cursors_opened(), sk0 = s.sp_skips();
    double t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double us = (now_us() - t0) / (double)S;
    std::printf("mode=%d runs=%u cells=%u range=%.3f cursors=%.2f skips=%.2f rows=%llu\n",
                mode, s.run_count(), (u32)s.cell_count(), us,
                (double)(s.cursors_opened() - co0) / S,
                (double)(s.sp_skips() - sk0) / S,
                (unsigned long long)got);
    std::remove(P);
    return 0;
}
EOF
g++ -std=c++17 -O2 -g -pthread -Iinclude /tmp/range_polish.cpp -o build/range_polish

OUT=backup/range-opt/knives.log
: > "$OUT"
for m in 0 1 2 3 4; do
  echo "== mode=$m ==" | tee -a "$OUT"
  for rep in 1 2; do
    ./build/range_polish 200000 500 2000 100 $m | tee -a "$OUT"
  done
done
echo "=== range_tax P2 ===" | tee -a "$OUT"
./build/range_tax 200000 500 2000 100 1 | tee -a "$OUT"
./build/range_tax 200000 500 2000 100 0 | tee -a "$OUT"
