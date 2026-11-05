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
    const u32 N = 50000, B = 500, VSZ = 32, S = 4000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rem167.db";
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
    std::printf("runs=%u cells=%u\n", s.run_count(), s.cell_count());

    for (u32 i = 0; i < 1000; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }

    // skip count before first emit
    u64 skips = 0, emits = 0, pages = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        Store::RGuard g(&s);
        Store::CursorRef ref(&s);
        Store::Cursor& cur = *ref;
        cur.open(&s, lo, lo + 4096, 0);
        u64 k; const u8* v; u16 vl; u32 ne = 0; bool first = true;
        u32 sk = 0;
        while (cur.next(k, v, vl) && ne < K) {
            if (first) { /* can't see skips from outside */ first = false; }
            ++ne;
        }
        emits += ne;
        (void)sk; (void)pages;
    }
    std::printf("emit_avg=%.2f\n", (double)emits / S);

    // timed slices
    double t_rg = 0, t_open = 0, t_first = 0, t_rest = 0, t_sel = 0, t_get = 0;
    u8 b[64]; u16 l = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        double t0 = now_us();
        { Store::RGuard g(&s); }
        t_rg += now_us() - t0;

        t0 = now_us();
        Store::RGuard g(&s);
        Store::CursorRef ref(&s);
        Store::Cursor& cur = *ref;
        cur.open(&s, lo, lo + 4096, 0);
        t_open += now_us() - t0;

        u64 k; const u8* v; u16 vl;
        t0 = now_us();
        bool ok = cur.next(k, v, vl);
        t_first += now_us() - t0;
        t0 = now_us();
        u32 ne = ok ? 1 : 0;
        while (ne < K && cur.next(k, v, vl)) ++ne;
        t_rest += now_us() - t0;
    }
    std::printf("slice RGuard=%.3f open=%.3f first_next=%.3f rest%d=%.3f sum=%.3f\n",
                t_rg / S, t_open / S, t_first / S, K - 1, t_rest / S,
                (t_rg + t_open + t_first + t_rest) / S);

    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        double t0 = now_us();
        u64 c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        t_sel += now_us() - t0;
    }
    for (u32 i = 0; i < S; ++i) {
        double t0 = now_us();
        s.get(keys[mix(i * 13 + 5) % n], b, sizeof b, &l);
        t_get += now_us() - t0;
    }
    std::printf("selectK=%.3f get=%.3f select/get=%.2f\n", t_sel / S, t_get / S, (t_sel / S) / (t_get / S));

    // lim1 vs lim100 vs lim0 window
    auto sel = [&](u32 lim, u64 win) {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + win, [&](u64, const u8*, u16) { ++c; return true; }, lim);
        }
        return (now_us() - t0) / S;
    };
    std::printf("lim1=%.3f lim10=%.3f lim100=%.3f win1key≈lim1/get\n",
                sel(1, 4096), sel(10, 4096), sel(100, 4096));

    // reuse Cursor without CursorRef churn: sticky open/seek
    Store::Cursor sticky;
    double t_stick = 0;
    {
        Store::RGuard g(&s);
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n];
            double t0 = now_us();
            sticky.open(&s, lo, lo + 4096, 0);
            u64 k; const u8* v; u16 vl; u32 ne = 0;
            while (sticky.next(k, v, vl) && ne < K) ++ne;
            t_stick += now_us() - t0;
        }
    }
    std::printf("sticky_open+nextK=%.3f (vs select≈%.3f)\n", t_stick / S, t_sel / S);

    // cold page vs hot: same key repeated
    {
        u64 lo = keys[n / 2];
        for (int i = 0; i < 100; ++i) {
            u64 c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        std::printf("hot_same_lo selectK=%.3f\n", (now_us() - t0) / S);
    }
    return 0;
}
