// frag_trace：外碎片时空分布探针（坑 136）
// Soft 路径下逐提交记录洞/PEND 时序；事件点完整转储地址分布。
// 用法：build/frag_trace [N=50000] [B=500] [VSZ=32] [out_dir=backup/frag-trace] [bf_mode=0] [bf_skip=0]
// 产出：
//   timeline.csv  — 每提交一行（时序）
//   holes.csv     — 事件点每条洞/PEND/DEF 一行（空间）
//   summary.txt   — 峰/直方图汇总
#define private public
#include "fcs.h"
#undef private
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace fcs;

static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}

struct HoleSnap {
    u32 holes = 0, maxh = 0, n_ge16 = 0, n_ge32 = 0, n_ge64 = 0, n_ge128 = 0, n_ge256 = 0;
    u64 free_tot = 0, free_ge64 = 0;
    u32 hist[12] = {};  // 2^k 桶：1,2,4,...,2048+
};

static HoleSnap scan_free(Store& s) {
    HoleSnap o;
    for (auto& kv : s.free_) {
        if (kv.first >= s.tail_) continue;
        u32 n = kv.second;
        if (kv.first + n > s.tail_) n = s.tail_ - kv.first;
        if (!n) continue;
        o.holes++;
        o.free_tot += n;
        if (n > o.maxh) o.maxh = n;
        if (n >= 16) ++o.n_ge16;
        if (n >= 32) ++o.n_ge32;
        if (n >= 64) { ++o.n_ge64; o.free_ge64 += n; }
        if (n >= 128) ++o.n_ge128;
        if (n >= 256) ++o.n_ge256;
        u32 b = 1, idx = 0;
        while (b < n && idx < 11) { b <<= 1; ++idx; }
        o.hist[idx]++;
    }
    return o;
}

static u64 sum_pend(Store& s) {
    u64 t = 0;
    for (auto& e : s.pending_free_) t += e.first;
    return t;
}
static u64 sum_def(Store& s) {
    u64 t = 0;
    for (auto& e : s.deferred_) t += e.first;
    return t;
}
static u32 pend_segs(Store& s) { return (u32)s.pending_free_.size(); }
static u32 def_segs(Store& s) { return (u32)s.deferred_.size(); }

static void dump_spatial(FILE* hf, u32 commit, const char* tag, Store& s) {
    for (auto& kv : s.free_) {
        if (kv.first >= s.tail_) continue;
        u32 n = kv.second;
        if (kv.first + n > s.tail_) n = s.tail_ - kv.first;
        if (!n) continue;
        const int mid = (kv.first + n < s.tail_) ? 1 : 0;
        std::fprintf(hf, "%u,%s,FREE,%u,%u,%d\n", commit, tag, kv.first, n, mid);
    }
    for (auto& e : s.pending_free_)
        std::fprintf(hf, "%u,%s,PEND,%u,%u,0\n", commit, tag, e.second, e.first);
    for (auto& e : s.deferred_)
        std::fprintf(hf, "%u,%s,DEF,%u,%u,0\n", commit, tag, e.second, e.first);
}

int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 VSZ = argc > 3 ? (u32)atoi(argv[3]) : 32;
    const char* out_dir = argc > 4 ? argv[4] : "backup/frag-trace";
    int bf_mode = argc > 5 ? atoi(argv[5]) : 0;
    int bf_skip = argc > 6 ? atoi(argv[6]) : 0;

    char tpath[512], hpath[512], spath[512];
    std::snprintf(tpath, sizeof tpath, "%s/timeline.csv", out_dir);
    std::snprintf(hpath, sizeof hpath, "%s/holes.csv", out_dir);
    std::snprintf(spath, sizeof spath, "%s/summary.txt", out_dir);
#ifdef _WIN32
    std::string mk = std::string("mkdir \"") + out_dir + "\" 2>nul";
#else
    std::string mk = std::string("mkdir -p \"") + out_dir + "\"";
#endif
    if (std::system(mk.c_str()) != 0) { /* out_dir 可能已存在 */ }

    const char* P = "build/frag_trace.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * (VSZ + 24) + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) { std::printf("open fail\n"); return 1; }
    s.configure_deploy(false, 150);
    if (bf_mode > 0) s.set_stream_best_fit_mode((u32)bf_mode);
    if (bf_skip > 0) s.set_stream_best_fit_skip_pct((u32)bf_skip);

    FILE* tf = std::fopen(tpath, "wb");
    FILE* hf = std::fopen(hpath, "wb");
    if (!tf || !hf) { std::printf("open csv fail\n"); return 1; }
    std::fprintf(tf,
                 "commit,phase,tail,peak,live_phys,live_logic,holes,maxh,free_tot,free_ge64,"
                 "n_ge16,n_ge32,n_ge64,n_ge128,n_ge256,pend_pages,pend_segs,def_pages,def_segs,"
                 "L0,L1,L2,L3,part_frags,hole_protect_skips,stream_best_fits,hw_reclaims,"
                 "d_tail,attr,event,h1,h2,h4,h8,h16,h32,h64,h128,h256,h512,h1024,h2048p\n");
    std::fprintf(hf, "commit,tag,kind,p0,np,mid\n");

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(VSZ, 7), val2(VSZ, 9);

    u32 commits = 0, peak = 0, peak_at = 0;
    u32 n_event = 0, n_starve = 0, n_spatial = 0;
    u64 prev_frags = 0, prev_skips = 0, prev_fits = 0;
    u32 prev_tail = s.tail_, prev_maxh = 0;
    bool prev_ge64 = true;

    auto emit = [&](const char* phase) {
        ++commits;
        HoleSnap hs = scan_free(s);
        u64 pend = sum_pend(s), def = sum_def(s);
        auto lh = s.lvl_hist();
        auto ms = s.maint_stats();
        u32 d_tail = s.tail_ > prev_tail ? s.tail_ - prev_tail : 0;
        const bool new_peak = s.tail_ > peak;
        if (new_peak) { peak = s.tail_; peak_at = commits; }

        const bool big = d_tail >= 64;
        const bool starve = hs.maxh < 64 && d_tail >= 64;
        const bool lag = pend >= 64 && hs.maxh < (u32)std::max<u64>(64ull, (u64)d_tail / 2);
        const bool frag_bump = s.part_frags() > prev_frags;
        const bool ge64_drop = prev_ge64 && hs.n_ge64 == 0 && hs.free_tot >= 64;
        const bool maxh_cross = (prev_maxh >= 64 && hs.maxh < 64) || (prev_maxh < 64 && hs.maxh >= 64);
        const char* attr = starve ? "HOLE_STARVE" : (lag ? "SB_LAG" : (big ? (hs.free_tot * 100 / (s.live_page_count() ? s.live_page_count() : 1) >= 30 ? "PRE_DIRTY" : "BIG") : "OK"));
        if (starve) ++n_starve;

        int event = 0;
        if (big || starve || lag || frag_bump || ge64_drop || maxh_cross || commits == 1)
            event = 1;
        if (s.tail_ == peak && big) event = 1;

        char evtag[32];
        if (starve) std::snprintf(evtag, sizeof evtag, "STARVE");
        else if (frag_bump) std::snprintf(evtag, sizeof evtag, "FRAG_BUMP");
        else if (ge64_drop) std::snprintf(evtag, sizeof evtag, "GE64_DROP");
        else if (maxh_cross) std::snprintf(evtag, sizeof evtag, "MAXH_X");
        else if (lag) std::snprintf(evtag, sizeof evtag, "SB_LAG");
        else if (big) std::snprintf(evtag, sizeof evtag, "BIG");
        else std::snprintf(evtag, sizeof evtag, "TICK");

        std::fprintf(tf,
                     "%u,%s,%u,%u,%llu,%llu,%u,%u,%llu,%llu,"
                     "%u,%u,%u,%u,%u,%llu,%u,%llu,%u,"
                     "%u,%u,%u,%u,%llu,%llu,%llu,%llu,"
                     "%u,%s,%d,"
                     "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                     commits, phase, s.tail_, peak,
                     (unsigned long long)s.live_page_count(), (unsigned long long)s.live_pages(),
                     hs.holes, hs.maxh, (unsigned long long)hs.free_tot, (unsigned long long)hs.free_ge64,
                     hs.n_ge16, hs.n_ge32, hs.n_ge64, hs.n_ge128, hs.n_ge256,
                     (unsigned long long)pend, pend_segs(s), (unsigned long long)def, def_segs(s),
                     lh[0], lh[1], lh[2], lh[3],
                     (unsigned long long)s.part_frags(), (unsigned long long)s.hole_protect_skips(),
                     (unsigned long long)s.stream_best_fits(), (unsigned long long)ms.hw_reclaims,
                     d_tail, attr, event,
                     hs.hist[0], hs.hist[1], hs.hist[2], hs.hist[3], hs.hist[4], hs.hist[5],
                     hs.hist[6], hs.hist[7], hs.hist[8], hs.hist[9], hs.hist[10], hs.hist[11]);

        if (event || new_peak) {
            ++n_event;
            // 峰与 ATTR 可能同提交：各打一份，避免 STARVE 被 PEAK 覆盖丢标签
            if (new_peak) {
                dump_spatial(hf, commits, "PEAK", s);
                ++n_spatial;
            }
            if (event) {
                dump_spatial(hf, commits, evtag, s);
                ++n_spatial;
            }
        }

        prev_frags = s.part_frags();
        prev_skips = s.hole_protect_skips();
        prev_fits = s.stream_best_fits();
        prev_tail = s.tail_;
        prev_maxh = hs.maxh;
        prev_ge64 = hs.n_ge64 > 0;
        (void)prev_skips; (void)prev_fits;
    };

    std::printf("# frag_trace N=%u B=%u VSZ=%u bf_mode=%d bf_skip=%d out=%s\n",
                N, B, VSZ, bf_mode, bf_skip, out_dir);

    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) { s.commit(); emit("ins"); }
    }
    s.commit(); emit("ins");

    for (u32 i = 0; i < N / 2; ++i) {
        u64 k = keys[mix(i + 999) % N];
        s.put(k, val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) { s.commit(); emit("upd"); }
    }
    s.commit(); emit("upd");

    { double w0 = prog_us();
      while (s.maint_pending() && prog_us() - w0 < 60e6) s.commit();
      s.commit(); s.commit(); }
    emit("fin");

    // 末态强制完整空间转储（峰点已在 new_peak 时转储）
    dump_spatial(hf, commits, "END", s);

    std::fclose(tf);
    std::fclose(hf);

    HoleSnap end = scan_free(s);
    FILE* sf = std::fopen(spath, "wb");
    if (sf) {
        std::fprintf(sf, "N=%u B=%u peak_tail=%u peak_at_commit=%u end_tail=%u\n", N, B, peak, peak_at, s.tail_);
        std::fprintf(sf, "live_logic=%llu live_phys=%llu peak_x=%.3f end_x=%.3f\n",
                     (unsigned long long)s.live_pages(), (unsigned long long)s.live_page_count(),
                     (double)peak / std::max<u64>(1, s.live_pages()),
                     (double)s.tail_ / std::max<u64>(1, s.live_pages()));
        std::fprintf(sf, "events=%u spatial_dumps=%u hole_starve_commits=%u\n", n_event, n_spatial, n_starve);
        std::fprintf(sf, "end holes=%u maxh=%u free=%llu free_ge64=%llu n_ge64=%u\n",
                     end.holes, end.maxh, (unsigned long long)end.free_tot,
                     (unsigned long long)end.free_ge64, end.n_ge64);
        std::fprintf(sf, "end hist[1,2,4,...,2048+]:");
        for (u32 i = 0; i < 12; ++i) std::fprintf(sf, " %u", end.hist[i]);
        std::fprintf(sf, "\n");
        std::fprintf(sf, "part_frags=%llu hole_protect_skips=%llu stream_best_fits=%llu\n",
                     (unsigned long long)s.part_frags(), (unsigned long long)s.hole_protect_skips(),
                     (unsigned long long)s.stream_best_fits());
        std::fprintf(sf, "files: %s\n%s\n", tpath, hpath);
        std::fclose(sf);
    }

    std::printf("== FRAG_TRACE peak=%u@c%u end_tail=%u events=%u starve=%u\n",
                peak, peak_at, s.tail_, n_event, n_starve);
    std::printf("== wrote %s\n== wrote %s\n== wrote %s\n", tpath, hpath, spath);
    std::remove(P);
    return 0;
}
