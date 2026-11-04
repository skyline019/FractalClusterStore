// 坑 147 续：动态整理策略消融（应用侧策略 + soft_consolidate；不改 Soft 默认）
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

// arm:
//  0 soft          — Soft，无 tidy
//  1 eager         — 每 commit 尽量 drain maybe_tidy（旧 soft_cons）
//  2 duty8         — 每 8 次 commit 最多 1 步
//  3 duty32        — 每 32 次 commit 最多 1 步
//  4 hyst          — runs>=16 才 tidy，降到 <=8 停；每 commit 最多 1 步
//  5 post          — 写完再 drain 最多 32 步（延迟整理）
//  6 polish        — 写完 polish
int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 S = argc > 3 ? (u32)atoi(argv[3]) : 2000;
    u32 K = argc > 4 ? (u32)atoi(argv[4]) : 100;
    int arm = argc > 5 ? atoi(argv[5]) : 0;
    const char* names[] = {"soft", "eager", "duty8", "duty32", "hyst", "post", "polish"};
    const char* aname = (arm >= 0 && arm <= 6) ? names[arm] : "unk";

    char path[128];
    std::snprintf(path, sizeof path, "/tmp/range_dyn_%u_%d.db", N, arm);
    std::remove(path);
    FileDev dev(path, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.configure_deploy(false, 150);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    if (arm >= 1 && arm <= 5) {
        s.set_soft_consolidate(true);
        s.set_tidy_runs(8, 96);
    }

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);

    u32 commits = 0, steps = 0, peak_used = 0;
    auto note_peak = [&]() {
        u32 u = s.used_pages();
        if (u > peak_used) peak_used = u;
    };
    auto step1 = [&]() {
        RC r = s.maybe_tidy();
        if (r == RC::MORE || r == RC::OK) {
            // MORE=还有；OK=本步做完或无压力。只在真正可能前进时计数：看 runs 变化太贵，按调用计
            ++steps;
        }
        note_peak();
        return r;
    };
    auto on_commit = [&]() {
        s.commit();
        ++commits;
        note_peak();
        if (arm == 1) {
            for (int i = 0; i < 64; ++i) {
                RC r = s.maybe_tidy();
                if (r != RC::MORE) { if (r == RC::OK) ++steps; break; }
                ++steps;
            }
        } else if (arm == 2) {
            if (commits % 8 == 0) step1();
        } else if (arm == 3) {
            if (commits % 32 == 0) step1();
        } else if (arm == 4) {
            u32 runs = s.run_count();
            if (runs >= 16) step1();
            // 降到 <=8 自然因 pressure 门限（tidy_runs=8）少触发；此处用 16 作开闸
        }
    };

    double t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), 32);
        if ((i + 1) % B == 0) on_commit();
    }
    on_commit();
    double ins = (now_us() - t0) / (double)N;

    t0 = now_us();
    for (u32 i = 0; i < N / 2; ++i) {
        s.put(keys[mix(i + 999) % N], val2.data(), 32);
        if ((i + 1) % B == 0) on_commit();
    }
    on_commit();
    double upd = (now_us() - t0) / (double)(N / 2);

    { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 60e6) { s.commit(); note_peak(); } s.commit(); }
    if (arm == 5) {
        for (int i = 0; i < 32; ++i) {
            RC r = s.maybe_tidy();
            if (r != RC::MORE) { if (r == RC::OK) ++steps; break; }
            ++steps;
            note_peak();
        }
    }
    if (arm == 6) s.polish();
    note_peak();

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

    std::printf("arm=%s N=%u runs=%u used=%u peak_used=%u live=%llu peak_x=%.3f "
                "insert=%.3f update=%.3f range=%.3f cursors=%.2f steps=%u commits=%u\n",
                aname, N, s.run_count(), s.used_pages(), peak_used,
                (unsigned long long)live, peak_x, ins, upd, rng,
                (double)(s.cursors_opened() - co0) / S, steps, commits);
    std::remove(path);
    return 0;
}
