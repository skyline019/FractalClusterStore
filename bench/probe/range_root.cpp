// 坑 146：Soft range 根源诊断 — lvl/fam hist · merge_diag · 实验臂（不改 Soft 默认）
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// arm: 0=baseline 1=cliff1 2=cheap_inf 3=lvl_t4 4=polish
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    int arm = argc > 5 ? atoi(argv[5]) : 0;
    const char* aname = "baseline";
    if (arm == 1) aname = "cliff1";
    else if (arm == 2) aname = "cheap_inf";
    else if (arm == 3) aname = "lvl_t4";
    else if (arm == 4) aname = "polish";

    char path[128];
    std::snprintf(path, sizeof path, "/tmp/range_root_%u_%d.db", N, arm);
    std::remove(path);
    FileDev dev(path, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) {
        std::printf("open fail\n");
        return 1;
    }
    s.configure_deploy(false, 150);
    s.set_merge_diag(true);
    s.clear_merge_diag();
    if (arm == 1) s.set_lvl_cliff(1);
    if (arm == 2) s.set_merge_cheap_pages(~0u);
    if (arm == 3) s.set_lvl_t(4);

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);

    double t_ins0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double ins_us = (now_us() - t_ins0) / (double)N;

    double t_upd0 = now_us();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double upd_us = (now_us() - t_upd0) / (double)(N / 2);

    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 60e6) s.commit(); s.commit(); }
    if (arm == 4) s.polish();

    auto lh = s.lvl_hist();
    auto fh = s.fam_hist();
    auto md = s.merge_diag_snap();

    u64 co0 = s.cursors_opened();
    double t0 = now_us();
    u64 got = 0;
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % N];
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
    }
    double rng = (now_us() - t0) / (double)S;

    std::printf("arm=%s N=%u runs=%u cells=%u max_segs=%u used=%u insert=%.3f update=%.3f "
                "range=%.3f cursors=%.2f rows=%llu\n",
                aname, N, s.run_count(), (u32)s.cell_count(), s.max_segs_per_cell(),
                s.used_pages(), ins_us, upd_us, rng,
                (double)(s.cursors_opened() - co0) / S, (unsigned long long)got);
    std::printf("lvl_hist=");
    for (u32 i = 0; i < lh.size(); ++i) std::printf("%s%u", i ? "," : "", lh[i]);
    std::printf(" fam_hist=");
    for (u32 i = 0; i < fh.size(); ++i) std::printf("%s%u", i ? "," : "", fh[i]);
    std::printf("\n");
    std::printf("md skip_need_t=%llu skip_budget=%llu skip_not_trigger=%llu "
                "skip_gap_promote=%llu skip_top_gap=%llu "
                "did_reclaim=%llu did_cheap=%llu did_cliff=%llu did_d=%llu did_merge=%llu\n",
                (unsigned long long)md.skip_need_t, (unsigned long long)md.skip_budget,
                (unsigned long long)md.skip_not_trigger,
                (unsigned long long)md.skip_gap_promote, (unsigned long long)md.skip_top_gap,
                (unsigned long long)md.did_reclaim, (unsigned long long)md.did_cheap,
                (unsigned long long)md.did_cliff, (unsigned long long)md.did_d,
                (unsigned long long)md.did_merge);
    std::remove(path);
    return 0;
}
