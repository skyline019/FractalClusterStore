// 坑 149：Idle/Post 写税刀消融（min_gap · run_hi）
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
using namespace fcs;

static u64 mix(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27;
    x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x;
}
static double now_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

// arm: 0 soft  1 eager  2 idle  3 post  4 polish
// argv: N B S K arm idle_us streak min_gap run_hi
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    int arm = argc > 5 ? atoi(argv[5]) : 0;
    u64 idle_us = argc > 6 ? (u64)atoll(argv[6]) : 8000;
    u32 streak = argc > 7 ? (u32)atoi(argv[7]) : 5;
    u32 gap = argc > 8 ? (u32)atoi(argv[8]) : 16;
    u32 run_hi = argc > 9 ? (u32)atoi(argv[9]) : 16;
    const char* names[] = {"soft", "eager", "idle", "post", "polish"};
    const char* aname = (arm >= 0 && arm <= 4) ? names[arm] : "unk";

    char path[128];
    std::snprintf(path, sizeof path, "/tmp/range_idle_%u_%d.db", N, arm);
    std::remove(path);
    FileDev dev(path, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.set_tidy_runs(8, 96);
    // 坑 151：Soft 默认已是 Post；soft/polish 基线显式 Off，再按臂覆盖
    s.set_soft_cons(Store::SoftCons::Off);
    if (arm == 1) s.set_soft_cons(Store::SoftCons::Eager);
    if (arm == 2 || arm == 3) {
        s.set_soft_cons(arm == 2 ? Store::SoftCons::Idle : Store::SoftCons::Post);
        s.set_soft_idle_max_us(idle_us);
        s.set_soft_idle_streak(streak);
        s.set_soft_idle_min_gap(gap);
        s.set_soft_idle_run_hi(run_hi);
    }

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);

    u32 peak_used = 0;
    auto note = [&]() {
        u32 u = s.used_pages();
        if (u > peak_used) peak_used = u;
    };
    auto eager_drain = [&]() {
        if (arm != 1) return;
        for (int i = 0; i < 64; ++i) {
            if (s.maybe_tidy() != RC::MORE) break;
        }
        note();
    };

    double t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) { s.commit(); note(); eager_drain(); }
    }
    s.commit(); note(); eager_drain();
    double ins = (now_us() - t0) / (double)N;

    t0 = now_us();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) { s.commit(); note(); eager_drain(); }
    }
    s.commit(); note(); eager_drain();
    double upd = (now_us() - t0) / (double)(N / 2);

    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 60e6) { s.commit(); note(); } s.commit(); note(); }
    if (arm == 3) {
        (void)s.soft_cons_repay(64);   // 坑 150：真·写后还债
        note();
    }
    if (arm == 4) s.polish();
    note();

    u64 live = s.live_page_count();
    double peak_x = live ? (double)peak_used / (double)live : 0;
    u64 co0 = s.cursors_opened();
    t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { return true; }, K);
    }
    double rng = (now_us() - t0) / (double)S;

    std::printf("arm=%s N=%u gap=%u run_hi=%u runs=%u debt=%u peak_x=%.3f "
                "insert=%.3f update=%.3f range=%.3f cursors=%.2f tidies=%llu\n",
                aname, N, gap, run_hi, s.run_count(), s.soft_idle_debt(), peak_x,
                ins, upd, rng, (double)(s.cursors_opened() - co0) / S,
                (unsigned long long)s.soft_idle_tidies());
    std::remove(path);
    return 0;
}
