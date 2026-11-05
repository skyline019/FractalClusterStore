// 坑 167 证据（历史）：fences+seek load 曾配对测得 mean on/off≈1.29× ⇒ Negate 已回滚。
// 本文件改为短窗剖面复现（emit_avg / open·first / cache），不再调用已删 set_range_fence_load。
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
    std::remove("/home/user/.fcs_r167p.db");
    FileDev dev("/home/user/.fcs_r167p.db", (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
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
    for (u32 i = 0; i < 2000; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    u64 emits = 0;
    double open_us = 0, first_us = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        Store::RGuard g(&s);
        Store::CursorRef ref(&s);
        Store::Cursor& cur = *ref;
        double t0 = now_us();
        cur.open(&s, lo, lo + 4096, 0);
        open_us += now_us() - t0;
        u64 k; const u8* v; u16 vl; u32 ne = 0;
        t0 = now_us();
        while (cur.next(k, v, vl) && ne < K) ++ne;
        first_us += now_us() - t0;
        emits += ne;
    }
    u64 h0 = s.cache_hits(), m0 = s.cache_misses();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }
    u64 h = s.cache_hits() - h0, m = s.cache_misses() - m0;
    std::printf("emit_avg=%.2f open=%.3f nextK=%.3f cache_hit=%.3f\n",
                (double)emits / S, open_us / S, first_us / S,
                (h + m) ? double(h) / (h + m) : 0.0);
    std::printf("NOTE fence+seek load Negate paired mean 1.29× (rolled back); see VERDICT_167.md\n");
    return 0;
}
