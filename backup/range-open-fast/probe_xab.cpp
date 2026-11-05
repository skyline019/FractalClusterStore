// 坑 158：同库交叉 A/B — 只测 range（排除写路径噪声）
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
static double range_us(Store& s, const std::vector<u64>& keys, u32 S, u32 K) {
    const u32 n = (u32)keys.size();
    double t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], cnt = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++cnt; return true; }, K);
    }
    return (now_us() - t0) / (double)S;
}
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rof_xab.db";
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
    // warmup
    s.set_range_open_fast(true);
    (void)range_us(s, keys, S, K);
    double on[5], off[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_open_fast(true);
        on[r] = range_us(s, keys, S, K);
        s.set_range_open_fast(false);
        off[r] = range_us(s, keys, S, K);
        std::printf("r%d on=%.3f off=%.3f on/off=%.3f\n", r + 1, on[r], off[r], on[r] / off[r]);
    }
    // median
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    double mo = med(on), mf = med(off);
    std::printf("median on=%.3f off=%.3f on/off=%.3f\n", mo, mf, mo / mf);
    return 0;
}
