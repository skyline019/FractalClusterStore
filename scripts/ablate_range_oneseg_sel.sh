#!/usr/bin/env bash
# 坑 164：oneseg select(limit=0) + prime 单段复用 · Soft 消融
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/range-oneseg-sel
mkdir -p "$OUT"

cat > /tmp/ablate_ros164.cpp <<'CPP'
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
static double full_us(Store& s, u32 reps) {
    double t0 = now_us();
    for (u32 r = 0; r < reps; ++r) {
        u64 c = 0;
        s.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++c; return true; }, 0);
    }
    return (now_us() - t0) / (double)reps;
}
static double get_us(Store& s, const std::vector<u64>& keys, u32 R) {
    const u32 n = (u32)keys.size();
    u8 b[64]; u16 l = 0;
    double t0 = now_us();
    for (u32 i = 0; i < R; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);
    return (now_us() - t0) / R;
}
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, S = 3000, K = 100, R = 80000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_ros164.db";
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
    std::printf("runs=%u cells=%u oneseg_sel=%d open_fast=%d\n", s.run_count(), s.cell_count(),
                s.range_oneseg_sel() ? 1 : 0, s.range_open_fast() ? 1 : 0);
    (void)range_us(s, keys, S, K);
    (void)get_us(s, keys, 5000);
    double on_s[5], off_s[5], on_f[5], off_f[5], on_g[5], off_g[5];
    for (int r = 0; r < 5; ++r) {
        // short: oneseg_sel gated by limit==0 ⇒ should be ~neutral; open_fast oneseg prime still on
        s.set_range_oneseg_sel(true);
        s.set_range_open_fast(true);
        on_s[r] = range_us(s, keys, S, K);
        on_f[r] = full_us(s, 6);
        on_g[r] = get_us(s, keys, R);
        s.set_range_oneseg_sel(false);
        s.set_range_open_fast(false);  // also disable 164b prime reuse
        off_s[r] = range_us(s, keys, S, K);
        off_f[r] = full_us(s, 6);
        // get oneseg path is always on when segs==1; re-enable open_fast for fair get? get independent
        s.set_range_open_fast(true);
        off_g[r] = get_us(s, keys, R);  // same get path — expect ~1
        std::printf("r%d short on=%.3f off=%.3f o/f=%.3f | full on=%.3f off=%.3f o/f=%.3f | get=%.3f/%.3f\n",
                    r + 1, on_s[r], off_s[r], on_s[r] / off_s[r],
                    on_f[r], off_f[r], on_f[r] / off_f[r], on_g[r], off_g[r]);
    }
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    std::printf("median short on/off=%.3f full on/off=%.3f get on=%.3f\n",
                med(on_s) / med(off_s), med(on_f) / med(off_f), med(on_g));
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_ros164.cpp -o /tmp/ablate_ros164 -lpthread
echo "=== pass1 ==="
/tmp/ablate_ros164 | tee "$OUT/pass1.txt"
echo "=== pass2 ==="
/tmp/ablate_ros164 | tee "$OUT/pass2.txt"

# xab short: open_fast on vs off only (164b), oneseg_sel stays true
cat > /tmp/ablate_prime164.cpp <<'CPP'
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
    const u32 N = 50000, B = 500, VSZ = 32, S = 3000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_prime164.db";
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
    auto rng = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        return (now_us() - t0) / S;
    };
    (void)rng();
    double on[5], off[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_open_fast(true);
        on[r] = rng();
        s.set_range_open_fast(false);
        off[r] = rng();
        std::printf("prime r%d on=%.3f off=%.3f on/off=%.3f\n", r + 1, on[r], off[r], on[r] / off[r]);
    }
    auto med = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    std::printf("median prime on/off=%.3f\n", med(on) / med(off));
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_prime164.cpp -o /tmp/ablate_prime164 -lpthread
echo "=== prime xab ==="
/tmp/ablate_prime164 | tee "$OUT/prime.txt"
echo "ABLATED_RANGE_ONESEG_SEL_OK"
