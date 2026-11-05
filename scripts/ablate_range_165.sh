#!/usr/bin/env bash
# 坑 165：Cur::reuse + 单段跳 may_have + get 免双 RGuard · Soft 短窗/get 消融
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/range-165
mkdir -p "$OUT"

cat > /tmp/ablate_r165.cpp <<'CPP'
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
    const u32 N = 50000, B = 500, VSZ = 32, S = 3000, K = 100, R = 100000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_r165.db";
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
    auto short_us = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        return (now_us() - t0) / S;
    };
    auto get_us = [&] {
        u8 b[64]; u16 l = 0;
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);
        return (now_us() - t0) / R;
    };
    auto full_us = [&] {
        double t0 = now_us();
        for (int k = 0; k < 6; ++k) {
            u64 c = 0;
            s.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++c; return true; }, 0);
        }
        return (now_us() - t0) / 6.0;
    };
    (void)short_us(); (void)get_us();
    // open_fast on (165 path) vs off (old try_prime / chains)
    double on_s[5], off_s[5], on_g[5], on_f[5], off_f[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_open_fast(true);
        s.set_range_oneseg_sel(true);
        on_s[r] = short_us();
        on_g[r] = get_us();
        on_f[r] = full_us();
        s.set_range_open_fast(false);
        s.set_range_oneseg_sel(false);
        off_s[r] = short_us();
        off_f[r] = full_us();
        std::printf("r%d short on=%.3f off=%.3f o/f=%.3f | full o/f=%.3f | get=%.3f\n",
                    r + 1, on_s[r], off_s[r], on_s[r] / off_s[r],
                    on_f[r] / off_f[r], on_g[r]);
    }
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    std::printf("median short on/off=%.3f full on/off=%.3f get=%.3f\n",
                med(on_s) / med(off_s), med(on_f) / med(off_f), med(on_g));
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_r165.cpp -o /tmp/ablate_r165 -lpthread
/tmp/ablate_r165 | tee "$OUT/pass1.txt"
/tmp/ablate_r165 | tee "$OUT/pass2.txt"
echo "ABLATED_165_OK"
