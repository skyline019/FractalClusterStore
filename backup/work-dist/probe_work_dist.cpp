// 坑 159：Soft sync 工作分布 — put / commit / flush_excl / maint / phase 分解
#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
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
    const u32 N = 50000, B = 500, VSZ = 32;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7), val2(VSZ, 3);
    const char* path = "/home/user/.fcs_wdist159.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);  // Soft sync Post
    s.clear_flush_timers();

    double put_us = 0, cmt_us = 0;
    auto ms0 = s.maint_stats();
    double t_all = now_us();
    for (u32 i = 0; i < n; ++i) {
        double t0 = now_us();
        if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) return 2;
        put_us += now_us() - t0;
        if ((i + 1) % B == 0) {
            t0 = now_us();
            s.commit();
            cmt_us += now_us() - t0;
        }
    }
    {
        double t0 = now_us();
        s.commit();
        cmt_us += now_us() - t0;
    }
    double wall = now_us() - t_all;
    auto ms = s.maint_stats();
    std::printf("=== Soft sync INSERT N=%u B=%u ===\n", n, B);
    std::printf("wall=%.0f put=%.0f (%.1f%%) commit=%.0f (%.1f%%)  µs/op wall=%.3f\n",
                wall, put_us, 100 * put_us / wall, cmt_us, 100 * cmt_us / wall, wall / n);
    std::printf("flush_excl_us=%llu (%.1f%% wall)\n",
                (unsigned long long)s.flush_excl_us(),
                100.0 * s.flush_excl_us() / wall);
    std::printf("maint: units=%llu level_us=%llu sweep_us=%llu rebal_us=%llu hw=%llu\n",
                (unsigned long long)(ms.units - ms0.units),
                (unsigned long long)(ms.level_us - ms0.level_us),
                (unsigned long long)(ms.sweep_us - ms0.sweep_us),
                (unsigned long long)(ms.rebal_us - ms0.rebal_us),
                (unsigned long long)(ms.hw_reclaims - ms0.hw_reclaims));
    std::printf("level share of wall=%.1f%%  excl+level≈%.1f%%\n",
                100.0 * (ms.level_us - ms0.level_us) / wall,
                100.0 * (s.flush_excl_us() + (ms.level_us - ms0.level_us)) / wall);
    std::printf("runs=%u cells=%u\n", s.run_count(), s.cell_count());

    // UPDATE half
    s.clear_flush_timers();
    ms0 = s.maint_stats();
    put_us = cmt_us = 0;
    t_all = now_us();
    for (u32 i = 0; i < n / 2; ++i) {
        double t0 = now_us();
        s.put(keys[mix(i + 999) % n], val2.data(), (u16)VSZ);
        put_us += now_us() - t0;
        if ((i + 1) % B == 0) {
            t0 = now_us();
            s.commit();
            cmt_us += now_us() - t0;
        }
    }
    {
        double t0 = now_us();
        s.commit();
        cmt_us += now_us() - t0;
    }
    wall = now_us() - t_all;
    ms = s.maint_stats();
    std::printf("\n=== Soft sync UPDATE n/2 ===\n");
    std::printf("wall=%.0f put=%.0f (%.1f%%) commit=%.0f (%.1f%%)  µs/op=%.3f\n",
                wall, put_us, 100 * put_us / wall, cmt_us, 100 * cmt_us / wall, wall / (n / 2.0));
    std::printf("flush_excl_us=%llu (%.1f%%) level_us=%llu (%.1f%%)\n",
                (unsigned long long)s.flush_excl_us(),
                100.0 * s.flush_excl_us() / wall,
                (unsigned long long)(ms.level_us - ms0.level_us),
                100.0 * (ms.level_us - ms0.level_us) / wall);

    // async Soft for I/O contrast
    std::remove(path);
    FileDev d2(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store a;
    a.open(&d2, nullptr, 2048);
    a.set_tidy_policy(Store::TidyPolicy::Soft);
    a.set_async_writes(true);
    a.set_io_threads(1);
    a.clear_flush_timers();
    ms0 = a.maint_stats();
    put_us = cmt_us = 0;
    t_all = now_us();
    for (u32 i = 0; i < n; ++i) {
        double t0 = now_us();
        a.put(keys[i], val.data(), (u16)VSZ);
        put_us += now_us() - t0;
        if ((i + 1) % B == 0) {
            t0 = now_us();
            a.commit(false);  // group: non-durable
            cmt_us += now_us() - t0;
        }
    }
    {
        double t0 = now_us();
        a.commit(true);
        cmt_us += now_us() - t0;
    }
    wall = now_us() - t_all;
    ms = a.maint_stats();
    std::printf("\n=== Soft async+group INSERT ===\n");
    std::printf("wall=%.0f put=%.0f (%.1f%%) commit=%.0f (%.1f%%) µs/op=%.3f\n",
                wall, put_us, 100 * put_us / wall, cmt_us, 100 * cmt_us / wall, wall / n);
    std::printf("flush_excl=%llu level_us=%llu\n",
                (unsigned long long)a.flush_excl_us(),
                (unsigned long long)(ms.level_us - ms0.level_us));
    return 0;
}
