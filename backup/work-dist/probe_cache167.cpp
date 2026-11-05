#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}
static double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(steady_clock::now().time_since_epoch()).count();
}
static void build(Store& s, std::vector<u64>& keys, u32 n, u32 B, const std::vector<u8>& val) {
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)val.size());
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
}
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, S = 4000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);

    auto run = [&](u32 cache_pages, const char* tag) {
        char path[128];
        std::snprintf(path, sizeof path, "/home/user/.fcs_c167_%u.db", cache_pages);
        std::remove(path);
        FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
        Store s;
        s.open(&dev, nullptr, cache_pages);
        s.set_tidy_policy(Store::TidyPolicy::Soft);
        build(s, keys, n, B, val);
        for (u32 i = 0; i < 2000; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        u64 h0 = s.cache_hits(), m0 = s.cache_misses();
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        double us = (now_us() - t0) / S;
        u64 h = s.cache_hits() - h0, m = s.cache_misses() - m0;
        std::printf("%s cache=%u select=%.3f hits=%llu miss=%llu rate=%.3f space=%u\n",
                    tag, cache_pages, us, (unsigned long long)h, (unsigned long long)m,
                    (h + m) ? double(h) / (h + m) : 0.0, s.used_pages());
    };
    run(256, "c256");
    run(512, "c512");
    run(1024, "c1k");
    run(2048, "c2k");
    run(8192, "c8k");
    return 0;
}
