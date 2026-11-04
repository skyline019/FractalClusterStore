// Soft 写后结构：runs by lvl · segs/cell · 单 scan 打开的链长（坑 145）
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
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
    int polish = argc > 3 ? atoi(argv[3]) : 0;
    const char* P = "/tmp/range_struct.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * 56ull + 8 * 1024 * 1024) / PAGE + 4096));
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
    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 30e6) s.commit(); s.commit(); }
    if (polish) s.polish();
    // dump via public-ish: run_count / cell_count; level hist needs friend — use scrub counters if any
    std::printf("polish=%d runs=%u cells=%u used=%u live_pages≈%llu\n",
                polish, s.run_count(), (u32)s.cell_count(), s.used_pages(),
                (unsigned long long)s.live_page_count());
    // sample 200 selects: record cursors opened delta as proxy for chain count
    u64 co0 = s.cursors_opened();
    u32 S = 200; u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, 100);
    }
    std::printf("sample cursors/scan=%.2f rows/scan=%.1f\n",
                (double)(s.cursors_opened() - co0) / S, (double)got / S);
    std::remove(P);
    return 0;
}
