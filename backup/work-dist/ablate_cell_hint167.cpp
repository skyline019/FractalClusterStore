#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <map>
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
    const u32 N = 50000, B = 500, VSZ = 32, S = 8000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    std::remove("/home/user/.fcs_ch167.db");
    FileDev dev("/home/user/.fcs_ch167.db", (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    std::printf("cells=%u runs=%u\n", s.cell_count(), s.run_count());

    // How often consecutive peer keys land in same cell? (approx via key spans)
    // Build cell boundaries by scanning with select lim1 and recording... skip
    // Time cells_.upper_bound only via many opens' fraction — already know open~0.2

    // Sequential lo vs random lo select
    auto sel_rand = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, 100);
        }
        return (now_us() - t0) / S;
    };
    auto sel_seq = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[i % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, 100);
        }
        return (now_us() - t0) / S;
    };
    (void)sel_rand();
    double r[3], q[3];
    for (int i = 0; i < 3; ++i) {
        r[i] = sel_rand();
        q[i] = sel_seq();
        std::printf("i%d rand=%.3f seq=%.3f seq/rand=%.3f\n", i + 1, r[i], q[i], q[i] / r[i]);
    }
    return 0;
}
