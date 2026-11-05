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
    const u32 N = 50000, B = 500, VSZ = 32, S = 6000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    std::remove("/home/user/.fcs_pf167f.db");
    FileDev dev("/home/user/.fcs_pf167f.db", (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    for (u32 i = 0; i < 3000; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    auto block = [&](bool pf) {
        s.set_prefetch(pf);
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        return (now_us() - t0) / S;
    };
    double on[5], off[5];
    for (int r = 0; r < 5; ++r) {
        if (r & 1) {
            off[r] = block(false);
            on[r] = block(true);
        } else {
            on[r] = block(true);
            off[r] = block(false);
        }
        std::printf("r%d on=%.3f off=%.3f o/f=%.3f\n", r + 1, on[r], off[r], on[r] / off[r]);
    }
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    std::printf("median pf on/off=%.3f\n", med(on) / med(off));
    return 0;
}
