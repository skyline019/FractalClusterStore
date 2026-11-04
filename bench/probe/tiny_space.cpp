// A2 小盘空间峰探针：RamDev 固定页预算，Soft 护洞 + space_accel 可消融。
// 用法：build/tiny_space [pages=512] [accel_pct=0] [N=0] [bf_mode=0] [bf_skip_pct=0]
//       [starve_fit=0] [starve_promote=0]
#define private public
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}
int main(int argc, char** argv) {
    u32 pages = argc > 1 ? (u32)atoi(argv[1]) : 512;
    u32 accel = argc > 2 ? (u32)atoi(argv[2]) : 0;  // Soft 默认已关 accel；探针默认 0
    u32 N = argc > 3 ? (u32)atoi(argv[3]) : 0;
    int bf_mode = argc > 4 ? atoi(argv[4]) : 0;
    int bf_skip = argc > 5 ? atoi(argv[5]) : 0;
    int starve_fit = argc > 6 ? atoi(argv[6]) : -1;       // -1=继承 Soft；0/1=显式
    int starve_promote = argc > 7 ? atoi(argv[7]) : -1;
    if (!N) N = pages >= 1024 ? 12000u : (pages >= 512 ? 8000u : 4000u);
    const u32 B = 25, VSZ = 200;
    static u8 arena[512 * PAGE];
    RamDev dev(pages);
    Store s;
    if (s.open(&dev, arena, 256) != RC::OK) { std::printf("open fail\n"); return 1; }
    s.configure_deploy(false, 150);
    s.set_space_accel_pct(accel);
    if (bf_mode > 0) s.set_stream_best_fit_mode((u32)bf_mode);
    if (bf_skip > 0) s.set_stream_best_fit_skip_pct((u32)bf_skip);
    if (starve_fit >= 0) s.set_part_starve_fit(starve_fit != 0);
    if (starve_promote >= 0) s.set_starve_promote(starve_promote != 0);
    std::vector<u8> val(VSZ, 7), val2(VSZ, 9);
    u32 peak = 0, nospace = 0, commits = 0;
    auto touch = [&] {
        if (s.used_pages() > peak) peak = s.used_pages();
    };
    for (u32 i = 0; i < N; ++i) {
        u64 k = mix(i + 1) % (N * 2) + 1;
        RC pr = s.put(k, val.data(), (u16)VSZ);
        if (pr == RC::NOSPACE) {
            RC cr = s.commit(); ++commits;
            if (cr == RC::NOSPACE) ++nospace;
            pr = s.put(k, val.data(), (u16)VSZ);
            if (pr == RC::NOSPACE) { ++nospace; break; }
        }
        if ((i + 1) % B == 0) {
            RC cr = s.commit(); ++commits;
            if (cr == RC::NOSPACE) ++nospace;
        }
        touch();
        if (nospace > 16) break;
    }
    for (u32 i = 0; i < N / 2; ++i) {
        u64 k = mix(i + 999) % (N * 2) + 1;
        (void)s.put(k, val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) {
            RC cr = s.commit(); ++commits;
            if (cr == RC::NOSPACE) ++nospace;
        }
        touch();
    }
    (void)s.commit();
    u64 live = s.live_pages();
    if (!live) live = 1;
    double px = (double)peak / (double)live;
    std::printf("TINY pages=%u accel=%u bf_mode=%u bf_skip=%u starve_fit=%d starve_promote=%d N=%u peak_tail=%u live_pages=%llu peak_x=%.3f space_accels=%llu stream_best_fits=%llu starve_fits=%llu starve_promotes=%llu nospace=%u commits=%u used=%u\n",
                pages, accel, s.stream_best_fit_mode(), s.stream_best_fit_skip_pct(),
                (int)s.part_starve_fit(), (int)s.starve_promote(), N, peak,
                (unsigned long long)live, px, (unsigned long long)s.space_accels(),
                (unsigned long long)s.stream_best_fits(),
                (unsigned long long)s.part_starve_fits(), (unsigned long long)s.starve_promotes(),
                nospace, commits, s.used_pages());
    return 0;
}
