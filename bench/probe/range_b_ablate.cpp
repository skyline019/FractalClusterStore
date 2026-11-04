// 坑 147：路径 B 可选项消融 — Soft vs Soft+soft_consolidate vs Hard vs polish
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

// arm: 0 Soft  1 Soft+soft_consolidate+maybe_tidy  2 Hard+maybe_tidy  3 Soft+end polish
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    int arm = argc > 5 ? atoi(argv[5]) : 0;
    const char* aname = "soft";
    if (arm == 1) aname = "soft_cons";
    else if (arm == 2) aname = "hard";
    else if (arm == 3) aname = "polish";

    char path[128];
    std::snprintf(path, sizeof path, "/tmp/range_b_%u_%d.db", N, arm);
    std::remove(path);
    FileDev dev(path, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    if (arm == 1) {
        s.set_tidy_policy(Store::TidyPolicy::Soft);
        s.set_soft_consolidate(true);
        s.set_tidy_runs(8, 96);   // Soft 压力阈值稍低，便于画像上迈步
    } else if (arm == 2) {
        s.set_tidy_policy(Store::TidyPolicy::Hard);
        s.set_tidy_runs(8, 8);
    }

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);

    auto drain_tidy = [&]() {
        if (arm != 1 && arm != 2) return;
        for (int i = 0; i < 64; ++i) {
            RC r = s.maybe_tidy();
            if (r != RC::MORE) break;
        }
    };

    double t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) { s.commit(); drain_tidy(); }
    }
    s.commit();
    drain_tidy();
    double ins = (now_us() - t0) / (double)N;

    t0 = now_us();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) { s.commit(); drain_tidy(); }
    }
    s.commit();
    drain_tidy();
    double upd = (now_us() - t0) / (double)(N / 2);

    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 60e6) s.commit(); s.commit(); }
    drain_tidy();
    if (arm == 3) s.polish();

    u32 peak_used = s.used_pages();
    u64 live = s.live_page_count();
    double peak_x = live ? (double)peak_used / (double)live : 0;

    u64 co0 = s.cursors_opened();
    t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double rng = (now_us() - t0) / (double)S;

    auto fh = s.fam_hist();
    std::printf("arm=%s N=%u runs=%u cells=%u used=%u live=%llu peak_x=%.3f "
                "insert=%.3f update=%.3f range=%.3f cursors=%.2f soft_cons=%d\n",
                aname, N, s.run_count(), (u32)s.cell_count(), peak_used,
                (unsigned long long)live, peak_x, ins, upd, rng,
                (double)(s.cursors_opened() - co0) / S, (int)s.soft_consolidate());
    std::printf("fam_hist=");
    for (size_t i = 0; i < fh.size(); ++i) std::printf("%s%u", i ? "," : "", fh[i]);
    std::printf("\n");
    std::remove(path);
    return 0;
}
