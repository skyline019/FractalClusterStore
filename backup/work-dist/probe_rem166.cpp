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
    const u32 N = 50000, B = 500, VSZ = 32, S = 4000, K = 100, R = 100000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rem166.db";
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
    std::printf("runs=%u cells=%u open_fast=%d page_view=%d oneseg=%d\n",
                s.run_count(), s.cell_count(), s.range_open_fast() ? 1 : 0,
                s.range_page_view() ? 1 : 0, s.range_oneseg_sel() ? 1 : 0);

    for (u32 i = 0; i < 800; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    u8 b[64]; u16 l = 0;
    for (u32 i = 0; i < 3000; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);

    double open_us = 0, next_us = 0, emit = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        double t0 = now_us();
        Store::RGuard g(&s);
        Store::CursorRef ref(&s);
        Store::Cursor& cur = *ref;
        cur.open(&s, lo, lo + 4096, 0);
        open_us += now_us() - t0;
        t0 = now_us();
        u64 k; const u8* v; u16 vl; u32 ne = 0;
        while (cur.next(k, v, vl) && ne < K) ++ne;
        next_us += now_us() - t0;
        emit += ne;
    }
    std::printf("split open=%.3f next=%.3f rows=%.2f open_frac=%.2f total=%.3f\n",
                open_us / S, next_us / S, emit / S, open_us / (open_us + next_us),
                (open_us + next_us) / S);

    auto sel = [&](u32 lim, u64 win) {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + win, [&](u64, const u8*, u16) { ++c; return true; }, lim);
        }
        return (now_us() - t0) / S;
    };
    std::printf("select API K100=%.3f lim1=%.3f\n", sel(K, 4096), sel(1, 4096));

    auto gmeas = [&](auto fn) {
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) fn(i);
        return (now_us() - t0) / R;
    };
    std::printf("get rand=%.3f seq=%.3f hot=%.3f\n",
                gmeas([&](u32 i) { s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l); }),
                gmeas([&](u32 i) { s.get(keys[i % n], b, sizeof b, &l); }),
                gmeas([&](u32) { s.get(keys[n / 2], b, sizeof b, &l); }));

    // page_view on/off same-DB for short
    double pv_on[3], pv_off[3];
    for (int r = 0; r < 3; ++r) {
        s.set_range_page_view(true);
        pv_on[r] = sel(K, 4096);
        s.set_range_page_view(false);
        pv_off[r] = sel(K, 4096);
    }
    s.set_range_page_view(true);
    std::printf("page_view on/off=%.3f %.3f %.3f\n",
                pv_on[0] / pv_off[0], pv_on[1] / pv_off[1], pv_on[2] / pv_off[2]);

    // shared_reads
    s.set_shared_reads(false);
    double g0 = gmeas([&](u32 i) { s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l); });
    double s0 = sel(K, 4096);
    s.set_shared_reads(true);
    double g1 = gmeas([&](u32 i) { s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l); });
    double s1 = sel(K, 4096);
    std::printf("shared_reads get %.3f→%.3f (%.3f×) sel %.3f→%.3f (%.3f×)\n",
                g0, g1, g1 / g0, s0, s1, s1 / s0);
    return 0;
}
