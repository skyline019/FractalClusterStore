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
    const u32 N = 50000, B = 500, VSZ = 32, S = 5000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    std::remove("/home/user/.fcs_pf167.db");
    FileDev dev("/home/user/.fcs_pf167.db", (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    for (u32 i = 0; i < 2000; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    // prefetch on/off paired
    double sum_on = 0, sum_off = 0;
    for (int pass = 0; pass < 5; ++pass) {
        double a = 0, b = 0;
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix((i + pass * 19) * 13 + 5) % n];
            u64 c = 0;
            s.set_prefetch(true);
            double t0 = now_us();
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
            a += now_us() - t0;
            c = 0;
            s.set_prefetch(false);
            t0 = now_us();
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
            b += now_us() - t0;
        }
        std::printf("pf paired pass%d on=%.3f off=%.3f o/f=%.3f\n", pass + 1, a / S, b / S, (a / S) / (b / S));
        sum_on += a / S;
        sum_off += b / S;
    }
    std::printf("pf paired mean on/off=%.3f ( <1 means on faster)\n", (sum_on / 5) / (sum_off / 5));
    s.set_prefetch(true);
    return 0;
}
