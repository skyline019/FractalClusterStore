#!/usr/bin/env bash
# 坑 161：range 翻页才预取 · Soft sync 消融（defer=on vs eager=old）
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/range-pf-defer
mkdir -p "$OUT"
make -j"$(nproc)" bins 2>&1 | tail -5

cat > /tmp/ablate_rpf161.cpp <<'CPP'
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
static double range_us(Store& s, const std::vector<u64>& keys, u32 S, u32 K, u64 win) {
    const u32 n = (u32)keys.size();
    double t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], cnt = 0;
        s.select(lo, lo + win, [&](u64, const u8*, u16) { ++cnt; return true; }, K);
    }
    return (now_us() - t0) / (double)S;
}
static double full_scan_us(Store& s, u32 reps) {
    double t0 = now_us();
    for (u32 r = 0; r < reps; ++r) {
        u64 c = 0;
        s.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++c; return true; }, 0);
    }
    return (now_us() - t0) / (double)reps;
}
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rpf161.db";
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
    std::printf("runs=%u cells=%u page_view=%d pf_eager=%d\n", s.run_count(), s.cell_count(),
                s.range_page_view() ? 1 : 0, s.range_pf_eager() ? 1 : 0);
    (void)range_us(s, keys, S, K, 4096);
    double defer_short[5], eager_short[5], defer_full[5], eager_full[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_pf_eager(false);
        defer_short[r] = range_us(s, keys, S, K, 4096);
        defer_full[r] = full_scan_us(s, 8);
        s.set_range_pf_eager(true);
        eager_short[r] = range_us(s, keys, S, K, 4096);
        eager_full[r] = full_scan_us(s, 8);
        std::printf("r%d short defer=%.3f eager=%.3f d/e=%.3f | full defer=%.3f eager=%.3f d/e=%.3f\n",
                    r + 1, defer_short[r], eager_short[r], defer_short[r] / eager_short[r],
                    defer_full[r], eager_full[r], defer_full[r] / eager_full[r]);
    }
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    double mds = med(defer_short), mes = med(eager_short);
    double mdf = med(defer_full), mef = med(eager_full);
    std::printf("median short defer=%.3f eager=%.3f defer/eager=%.3f\n", mds, mes, mds / mes);
    std::printf("median full  defer=%.3f eager=%.3f defer/eager=%.3f\n", mdf, mef, mdf / mef);
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_rpf161.cpp -o /tmp/ablate_rpf161 -lpthread

echo "=== pass1 ==="
/tmp/ablate_rpf161 | tee "$OUT/pass1.txt"
echo "=== pass2 ==="
/tmp/ablate_rpf161 | tee "$OUT/pass2.txt"
echo "ABLATED_RANGE_PF_DEFER_OK"
