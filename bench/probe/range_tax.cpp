// Soft A1 画像后量 range：cursors/scan · µs/scan（坑 145 range 读刀基线）
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; }
static double now_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 200000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    int p2 = argc > 5 ? atoi(argv[5]) : 1;
    const char* P = "build/range_tax.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * 56ull + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) { std::printf("open fail\n"); return 1; }
    s.configure_deploy(false, 150);
    if (!p2) { s.set_part_starve_fit(false); s.set_starve_promote(false); }
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
    u64 co0 = s.cursors_opened(), sk0 = s.sp_skips();
    double t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double us = (now_us() - t0) / (double)S;
    std::printf("# range_tax N=%u p2=%d runs=%u cells=%u peak_tail≈%u\n",
                N, p2, s.run_count(), (u32)s.cell_count(), s.used_pages());
    std::printf("# range: %.3f µs/scan rows=%llu cursors/scan=%.2f sp_skips/scan=%.2f fits=%llu prom=%llu\n",
                us, (unsigned long long)got,
                (double)(s.cursors_opened() - co0) / S,
                (double)(s.sp_skips() - sk0) / S,
                (unsigned long long)s.part_starve_fits(),
                (unsigned long long)s.starve_promotes());
    std::remove(P);
    return 0;
}
