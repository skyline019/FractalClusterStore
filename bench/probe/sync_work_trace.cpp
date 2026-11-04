// Soft sync 工作节奏追踪（坑 148）：每提交墙钟 + maint 增量 → 峰/谷分布
// 用法：build/sync_work_trace [N=50000] [B=500] [out_dir=backup/sync-work]
#include "fcs.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
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

int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    const char* out_dir = argc > 3 ? argv[3] : "backup/sync-work";
#ifdef _WIN32
    std::string mk = std::string("mkdir \"") + out_dir + "\" 2>nul";
#else
    std::string mk = std::string("mkdir -p \"") + out_dir + "\"";
#endif
    (void)std::system(mk.c_str());

    char tpath[512], spath[512];
    std::snprintf(tpath, sizeof tpath, "%s/work_timeline.csv", out_dir);
    std::snprintf(spath, sizeof spath, "%s/work_summary.txt", out_dir);

    char dbpath[256];
    std::snprintf(dbpath, sizeof dbpath, "%s/sync_work.db", out_dir);
    std::remove(dbpath);
    FileDev dev(dbpath, (PageId)((N * 56ull + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) {
        std::printf("open fail\n");
        return 1;
    }
    s.configure_deploy(false, 150);  // Soft sync

    FILE* tf = std::fopen(tpath, "wb");
    if (!tf) { std::printf("csv fail\n"); return 1; }
    std::fprintf(tf,
                 "commit,phase,commit_us,batch_us,runs,tail,live,peak_tail,peak_x,"
                 "d_units,d_slices,d_level_us,d_sweep_us,d_rebal_us,d_hw,d_fits,d_prom,"
                 "d_parts,maint_pending,unit_max_us,attr\n");

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(32, 7), val2(32, 9);

    auto ms0 = s.maint_stats();
    u64 fits0 = s.part_starve_fits(), prom0 = s.starve_promotes(), parts0 = s.merge_parts();
    u32 commits = 0, peak_tail = 0, peak_commit = 0;
    double peak_cmt_us = 0;
    u32 peak_cmt_at = 0;
    std::vector<double> all_cmt;

    auto snap_deltas = [&](auto& ms) {
        struct D {
            u64 units, slices, level_us, sweep_us, rebal_us, hw, fits, prom, parts;
        } d;
        d.units = ms.units - ms0.units;
        d.slices = ms.slices - ms0.slices;
        d.level_us = ms.level_us - ms0.level_us;
        d.sweep_us = ms.sweep_us - ms0.sweep_us;
        d.rebal_us = ms.rebal_us - ms0.rebal_us;
        d.hw = ms.hw_reclaims - ms0.hw_reclaims;
        d.fits = s.part_starve_fits() - fits0;
        d.prom = s.starve_promotes() - prom0;
        d.parts = s.merge_parts() - parts0;
        ms0 = ms;
        fits0 = s.part_starve_fits();
        prom0 = s.starve_promotes();
        parts0 = s.merge_parts();
        return d;
    };

    auto emit = [&](const char* phase, double commit_us, double batch_us) {
        ++commits;
        auto ms = s.maint_stats();
        auto d = snap_deltas(ms);
        if (s.used_pages() > peak_tail) {
            peak_tail = s.used_pages();
            peak_commit = commits;
        }
        u64 live = s.live_page_count();
        double px = live ? (double)peak_tail / (double)live : 0;
        all_cmt.push_back(commit_us);
        if (commit_us > peak_cmt_us) {
            peak_cmt_us = commit_us;
            peak_cmt_at = commits;
        }

        // 工作标签：墙钟 + 维护增量
        const char* attr = "IDLE";
        if (commit_us >= 2000 || d.level_us + d.sweep_us + d.rebal_us >= 1000)
            attr = "PEAK";
        else if (commit_us >= 500 || d.units > 0 || d.slices > 0 || d.fits || d.prom || d.parts || d.hw)
            attr = "BUSY";
        else if (commit_us >= 100)
            attr = "WARM";
        // else IDLE

        std::fprintf(tf,
                     "%u,%s,%.1f,%.1f,%u,%u,%llu,%u,%.3f,"
                     "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                     "%llu,%d,%llu,%s\n",
                     commits, phase, commit_us, batch_us, s.run_count(), s.used_pages(),
                     (unsigned long long)live, peak_tail, px,
                     (unsigned long long)d.units, (unsigned long long)d.slices,
                     (unsigned long long)d.level_us, (unsigned long long)d.sweep_us,
                     (unsigned long long)d.rebal_us, (unsigned long long)d.hw,
                     (unsigned long long)d.fits, (unsigned long long)d.prom,
                     (unsigned long long)d.parts, (int)s.maint_pending(),
                     (unsigned long long)ms.unit_max_us, attr);
    };

    auto do_batch = [&](const char* phase, u32 from, u32 to, bool upd) {
        double b0 = now_us();
        for (u32 i = from; i < to; ++i) {
            if (!upd) s.put(keys[i], val.data(), 32);
            else s.put(keys[mix(i + 999) % N], val2.data(), 32);
        }
        double batch_us = now_us() - b0;
        double c0 = now_us();
        s.commit();
        double commit_us = now_us() - c0;
        emit(phase, commit_us, batch_us);
    };

    std::printf("# sync_work_trace Soft sync N=%u B=%u out=%s\n", N, B, out_dir);

    for (u32 i = 0; i < N; i += B) {
        u32 to = std::min(i + B, N);
        do_batch("ins", i, to, false);
    }
    {
        double c0 = now_us();
        s.commit();
        emit("ins", now_us() - c0, 0);
    }

    for (u32 i = 0; i < N / 2; i += B) {
        u32 to = std::min(i + B, N / 2);
        do_batch("upd", i, to, true);
    }
    {
        double c0 = now_us();
        s.commit();
        emit("upd", now_us() - c0, 0);
    }

    {
        double w0 = now_us();
        while (s.maint_pending() && now_us() - w0 < 60e6) {
            double c0 = now_us();
            s.commit();
            emit("drain", now_us() - c0, 0);
        }
        double c0 = now_us();
        s.commit();
        emit("fin", now_us() - c0, 0);
    }

    std::fclose(tf);

    // 摘要：分位 + 峰谷 streak
    std::vector<double> sorted = all_cmt;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> double {
        if (sorted.empty()) return 0;
        size_t i = (size_t)(p * (sorted.size() - 1));
        return sorted[i];
    };
    u32 n_peak = 0, n_busy = 0, n_warm = 0, n_idle = 0;
    // 重读 csv 太重：再扫一遍用阈值近似（与 emit 一致）
    // 直接从 all_cmt 不够 attr；用文件尾 summary 只写分位，attr 计数留给 analyze 脚本

    FILE* sf = std::fopen(spath, "wb");
    if (sf) {
        std::fprintf(sf, "N=%u B=%u commits=%u\n", N, B, commits);
        std::fprintf(sf, "commit_us p50=%.1f p90=%.1f p99=%.1f max=%.1f@c%u\n",
                     pct(0.50), pct(0.90), pct(0.99), peak_cmt_us, peak_cmt_at);
        std::fprintf(sf, "space peak_tail=%u@c%u live_end=%llu\n",
                     peak_tail, peak_commit, (unsigned long long)s.live_page_count());
        std::fprintf(sf, "runs_end=%u unit_max_us=%llu\n",
                     s.run_count(), (unsigned long long)s.maint_stats().unit_max_us);
        std::fclose(sf);
    }

    std::printf("# commits=%u commit_us p50=%.1f p90=%.1f p99=%.1f max=%.1f@c%u peak_tail=%u@c%u\n",
                commits, pct(0.50), pct(0.90), pct(0.99), peak_cmt_us, peak_cmt_at,
                peak_tail, peak_commit);
    std::remove(dbpath);
    (void)n_peak; (void)n_busy; (void)n_warm; (void)n_idle;
    return 0;
}
