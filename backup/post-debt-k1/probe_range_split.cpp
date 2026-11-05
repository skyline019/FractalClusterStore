// 坑 158：select 分 open / next 墙钟（Post repay 后）
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
    const u32 N = 50000, B = 500, VSZ = 32, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rng158.db";
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
    std::printf("after repay runs=%u cells=%u\n", s.run_count(), s.cell_count());

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
        u64 k; const u8* v; u16 l; u32 nemit = 0;
        while (cur.next(k, v, l) && nemit < K) { ++nemit; }
        next_us += now_us() - t0;
        emit += nemit;
    }
    std::printf("S=%u K=%u open_us/scan=%.3f next_us/scan=%.3f rows/scan=%.2f open_frac=%.2f\n",
                S, K, open_us / S, next_us / S, emit / S, open_us / (open_us + next_us));

    // warm: reuse one Cursor via seek (LMDB-like)
    open_us = next_us = emit = 0;
    {
        Store::RGuard g(&s);
        Store::CursorRef ref(&s);
        Store::Cursor& cur = *ref;
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n];
            double t0 = now_us();
            cur.open(&s, lo, lo + 4096, 0);
            open_us += now_us() - t0;
            t0 = now_us();
            u64 k; const u8* v; u16 l; u32 nemit = 0;
            while (cur.next(k, v, l) && nemit < K) { ++nemit; }
            next_us += now_us() - t0;
            emit += nemit;
        }
    }
    std::printf("reuse_obj open=%.3f next=%.3f rows/scan=%.2f open_frac=%.2f\n",
                open_us / S, next_us / S, emit / S, open_us / (open_us + next_us));
    return 0;
}
