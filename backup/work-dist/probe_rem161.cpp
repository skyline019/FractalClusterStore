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
    const u32 N = 50000, B = 500, VSZ = 32, S = 2000, K = 100, R = 50000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rem161.db";
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
    std::printf("runs=%u cells=%u page_view=%d open_fast=%d pf_eager=%d\n", s.run_count(), s.cell_count(),
                s.range_page_view() ? 1 : 0, s.range_open_fast() ? 1 : 0, s.range_pf_eager() ? 1 : 0);

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
        u64 k;
        const u8* v;
        u16 l;
        u32 nemit = 0;
        while (cur.next(k, v, l) && nemit < K) ++nemit;
        next_us += now_us() - t0;
        emit += nemit;
    }
    std::printf("select split open=%.3f next=%.3f rows=%.2f open_frac=%.2f total=%.3f\n",
                open_us / S, next_us / S, emit / S, open_us / (open_us + next_us),
                (open_us + next_us) / S);

    double t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    std::printf("select API=%.3f\n", (now_us() - t0) / S);

    t0 = now_us();
    for (u32 i = 0; i < R; ++i) {
        u8 b[64];
        u16 l = 0;
        s.get(keys[mix(i * 7 + 1) % n], b, sizeof b, &l);
    }
    std::printf("get=%.3f\n", (now_us() - t0) / R);

    s.set_prefetch(false);
    t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    double no_pf = (now_us() - t0) / S;
    s.set_prefetch(true);
    t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    double pf = (now_us() - t0) / S;
    std::printf("range prefetch_off=%.3f on=%.3f off/on=%.3f\n", no_pf, pf, no_pf / pf);

    // limit=1 vs K=100 (peers windows often 1 row)
    t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, 1);
    }
    std::printf("select limit1=%.3f\n", (now_us() - t0) / S);
    return 0;
}
