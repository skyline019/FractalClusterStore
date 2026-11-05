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
    const u32 N = 50000, B = 500, VSZ = 32, S = 3000, K = 100, R = 80000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rem164.db";
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
    std::printf("runs=%u cells=%u segs=%u page_view=%d open_fast=%d pf_eager=%d tip=%d\n",
                s.run_count(), s.cell_count(), s.seg_max(),
                s.range_page_view() ? 1 : 0, s.range_open_fast() ? 1 : 0,
                s.range_pf_eager() ? 1 : 0, s.flush_tip() ? 1 : 0);

    // warmup
    for (u32 i = 0; i < 500; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    u8 b[64]; u16 l = 0;
    for (u32 i = 0; i < 2000; ++i) s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);

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
        u64 k; const u8* v; u16 vl; u32 nemit = 0;
        while (cur.next(k, v, vl) && nemit < K) ++nemit;
        next_us += now_us() - t0;
        emit += nemit;
    }
    std::printf("select split open=%.3f next=%.3f rows=%.2f open_frac=%.2f total=%.3f\n",
                open_us / S, next_us / S, emit / S, open_us / (open_us + next_us),
                (open_us + next_us) / S);

    auto meas_sel = [&](u32 lim, u64 win) {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + win, [&](u64, const u8*, u16) { ++c; return true; }, lim);
        }
        return (now_us() - t0) / S;
    };
    std::printf("select K100 win4k=%.3f lim1=%.3f win1=%.3f\n",
                meas_sel(K, 4096), meas_sel(1, 4096), meas_sel(1, 1));

    // get: random / seq / hot1 / get_at reuse view
    auto meas_get = [&](auto fn) {
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) fn(i);
        return (now_us() - t0) / R;
    };
    double g_rand = meas_get([&](u32 i) { s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l); });
    double g_seq = meas_get([&](u32 i) { s.get(keys[i % n], b, sizeof b, &l); });
    u64 hk = keys[n / 2];
    double g_hot = meas_get([&](u32) { s.get(hk, b, sizeof b, &l); });
    // point-like select as get substitute
    double g_sel1 = meas_sel(1, 1);
    std::printf("get rand=%.3f seq=%.3f hot1=%.3f select_point=%.3f\n", g_rand, g_seq, g_hot, g_sel1);

    // ensure_ready / RGuard tax: already-ready path
    {
        double t0 = now_us();
        for (u32 i = 0; i < R; ++i) {
            Store::RGuard g(&s);
            (void)s.open_vers();
        }
        std::printf("RGuard+open_vers=%.3f\n", (now_us() - t0) / R);
    }
    return 0;
}
