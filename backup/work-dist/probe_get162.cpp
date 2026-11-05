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
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, R = 100000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_get162.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    u8 b[64];
    u16 l = 0;
    for (u32 i = 0; i < 2000; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);
    auto run = [&](const char* tag) {
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);
        std::printf("%s get=%.3f\n", tag, (now_us() - t0) / R);
    };
    run("base");
    {
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) s.get(keys[i % n], b, sizeof b, &l);
        std::printf("seq get=%.3f\n", (now_us() - t0) / R);
    }
    // same key hammer
    {
        double t0 = now_us();
        u64 k = keys[n / 2];
        for (u32 i = 0; i < R; ++i) s.get(k, b, sizeof b, &l);
        std::printf("hot1 get=%.3f\n", (now_us() - t0) / R);
    }
    std::printf("runs=%u cells=%u segs_max=%u\n", s.run_count(), s.cell_count(), s.seg_max());
    return 0;
}
