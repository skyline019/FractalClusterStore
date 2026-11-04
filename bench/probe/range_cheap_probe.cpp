// Soft sync：cheap_pages 消融对 range / insert（坑 145）
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
    u32 cheap = argc > 1 ? (u32)atoi(argv[1]) : 1024;
    u32 N = 50000, B = 500, R = 50000, S = 2000, K = 100;
    const char* P = "/home/user/.fcs_range_cheap.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * 56ull + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    s.set_merge_cheap_pages(cheap);
    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);
    double t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double ins = (now_us() - t0) / N;
    t0 = now_us();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double upd = (now_us() - t0) / (N / 2.0);
    t0 = now_us();
    u32 hits = 0; u8 buf[64]; u16 l = 0;
    for (u32 i = 0; i < R; ++i)
        if (s.get(keys[mix(i * 7 + 1) % N], buf, sizeof buf, &l) == RC::OK) ++hits;
    double get = (now_us() - t0) / R;
    (void)hits;
    u64 co0 = s.cursors_opened();
    t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double rng = (now_us() - t0) / S;
    std::printf("cheap=%u runs=%u insert=%.3f update=%.3f get=%.3f range=%.3f cursors/scan=%.2f\n",
                cheap, s.run_count(), ins, upd, get, rng,
                (double)(s.cursors_opened() - co0) / S);
    std::remove(P);
    return 0;
}
