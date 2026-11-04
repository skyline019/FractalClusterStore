// Soft A1 峰值归因 + alive/dead 页清单（坑 126/127）：
// Soft + 大涨 ATTR；峰/末态 dump_live_inv（页无 live bit —— 按 run/段扫记录归类）。
// 用法：build/soft_a1 [N=200000] [B=500] [VSZ=32] [quiet=1] [inv=1] [p1=0] [bf_mode=0] [bf_skip_pct=0]
//       [starve_fit=0] [starve_promote=0] [starve_floor=0] [max_slices=0] [max_prom=0] [prom_min_pend=0]
//       [soft_cons=-1]
//       后四项 0 = 引擎宏默认（坑 140 消融）；soft_cons -1=继承 Soft 默认 0=Off 3=Post（坑 151）
// bf_mode：0关 1全量 2仅小np；bf_skip_pct：≥used/live% 旁路 bf
// starve_*：坑 138 P2 α/β（默认关）
#define private public
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
using namespace fcs;

static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}

struct Snap {
    u32 tail = 0, peak = 0;
    u64 live = 0, free_tot = 0, pend = 0, dead_est = 0, owned = 0;
    u32 holes = 0, max_hole = 0;
    u32 L0 = 0, L1 = 0, L2 = 0, L3 = 0;
};

static Snap snap(Store& s) {
    Snap o;
    o.tail = s.tail_;
    o.live = s.live_page_count();   // 物理占用口径（≠ live_pages 归一估）
    for (auto& kv : s.free_) {
        if (kv.first >= s.tail_) continue;
        u32 n = kv.second;
        if (kv.first + n > s.tail_) n = s.tail_ - kv.first;
        o.holes++; o.free_tot += n;
        if (n > o.max_hole) o.max_hole = n;
    }
    for (auto& p : s.pending_free_) o.pend += p.first;   // emplace(np, p0)
    auto h = s.lvl_hist();
    o.L0 = h[0]; o.L1 = h[1]; o.L2 = h[2]; o.L3 = h[3];
    for (auto& kv : s.runs_) {
        Run* r = kv.second;
        if (!r || !r->npages) continue;
        u32 blk = s.run_block(r);
        o.owned += blk;
        if (r->nrec && r->live_recs < r->nrec)
            o.dead_est += (u64)blk * (r->nrec - r->live_recs) / r->nrec;
    }
    return o;
}

// 页级清单：引擎没有 per-page live bit；数据页靠段键域 + newer_has/range_hidden 扫记录。
// 分类：FREE / PENDING / DEFERRED / MAN / OVFL / META / TRIM_ANCHOR /
//       DATA_ALL_LIVE / DATA_MIXED / DATA_ALL_DEAD / UNCLAIMED
static void dump_live_inv(Store& s, const char* tag) {
    s.recount_all_unknown();
    const u32 tail = s.tail_;
    if (tail < 2) { std::printf("== LIVE_INV %s empty\n", tag); return; }
    enum : u8 {
        U = 0, FREE, PEND, DEF, MAN, OVFL, META, ANCHOR,
        LIVE, MIX, DEAD
    };
    std::vector<u8> kind(tail, U);
    auto paint = [&](u32 p0, u32 n, u8 k) {
        for (u32 i = 0; i < n; ++i) {
            u32 p = p0 + i;
            if (p < 2 || p >= tail) continue;
            if (kind[p] == U || k == PEND || k == DEF) kind[p] = k;
        }
    };
    for (auto& kv : s.free_) {
        if (kv.first >= tail) continue;
        u32 n = kv.second;
        if (kv.first + n > tail) n = tail - kv.first;
        paint(kv.first, n, FREE);
    }
    for (auto& e : s.pending_free_) paint(e.second, e.first, PEND);
    for (auto& e : s.deferred_) paint(e.second, e.first, DEF);
    { u32 pid = s.man_last_;
      for (u32 i = 0; i < s.nman_ && pid != NIL; ++i) {
          paint(pid, 1, MAN);
          const u8* p = s.page(pid);
          pid = p ? rd32(p + 4) : (u32)NIL;
      }
    }
    struct RunRow {
        u32 page0, lvl, np, blk, nrec, live_exact, trim;
        u32 pg_live = 0, pg_mix = 0, pg_dead = 0, pg_anchor = 0, pg_meta = 0;
        u32 dead_lo = 0, dead_mid = 0, dead_hi = 0;
        u32 mix_pages_dense = 0, mix_pages_sparse = 0;  // MIX 页 dead_frac ≥25% vs <25%
        u64 streak_sum = 0; u32 streak_n = 0;            // MIX 页最长连续死 streak
    };
    std::vector<RunRow> rows;
    u64 rec_live = 0, rec_dead = 0, rec_tot = 0;
    u32 dens_hist[5] = {};   // MIX 页 dead_frac 桶
    for (auto& kv : s.runs_) {
        Run* r = kv.second;
        if (!r) continue;
        s.ensure_meta(r);
        s.ensure_ovfl_loc(r);
        for (auto& e : r->ovfl_loc) {
            const u8* p = s.page(e.first);
            if (!p) continue;
            auto d = Store::desc_of(pg_val_at(p, e.second));
            if (d.npages && d.pg0 >= 2) paint(d.pg0, d.npages, OVFL);
        }
        if (r->mnpg) paint(r->page0 + r->npages, r->mnpg, META);
        if (r->aux_npg) paint(r->page0 + r->npages + r->mnpg, r->aux_npg, META);

        RunRow row{};
        row.page0 = r->page0; row.lvl = r->lvl; row.np = r->npages;
        row.blk = Store::run_block(r); row.nrec = r->nrec; row.trim = r->trim;
        row.live_exact = s.exact_live_recs(r);
        rec_tot += r->nrec; rec_live += row.live_exact;
        if (r->nrec >= row.live_exact) rec_dead += r->nrec - row.live_exact;

        // 段覆盖的页 → 逐页数 live 记录
        struct Span { u64 klo, khi; u32 p0, p1; };
        std::vector<Span> spans;
        const SegKey sk = seg_key(r->seq, r->page0);
        for (auto& ck : s.cells_) {
            auto f = ck.second->segs.find(sk);
            if (f == ck.second->segs.end()) continue;
            const Seg& sg = f->second;
            if (sg.p0 >= sg.p1 || sg.klo >= sg.khi) continue;
            spans.push_back({sg.klo, sg.khi, sg.p0, std::min(sg.p1, r->npages)});
        }
        std::vector<u32> plive(r->npages, 0), pseen(r->npages, 0), pdead(r->npages, 0);
        // 页内空间：三分位死记录数 + 最长连续死 streak（键序=页内物理序）
        std::vector<u32> d_lo(r->npages, 0), d_mid(r->npages, 0), d_hi(r->npages, 0), streak_max(r->npages, 0);
        for (const Span& sp : spans) {
            bool past = false;
            for (u32 i = sp.p0; i < sp.p1 && !past; ++i) {
                const u8* src = s.page(r->page0 + i);
                if (!src) continue;
                u16 used = rd16(src + 34);
                if (used < DHDR || used > PAGE) continue;
                u8 pf = pg_pfx(src);
                u32 off = DHDR;
                std::vector<u8> alive_seq;   // 1=live 0=dead，仅本段键域
                while (off + pg_rec_size(pf, 0) <= used) {
                    RecHdr h; rd_rec(src, off, h);
                    u32 rs = pg_rec_size(pf, h.len);
                    if (off + rs > used) break;
                    off += rs;
                    if (h.key < sp.klo) continue;
                    if (h.key >= sp.khi) { past = true; break; }
                    ++pseen[i];
                    bool liv = !s.range_hidden(h.key, r->ver, 0xFFFFFFFFu) &&
                               !s.newer_has(h.key, r->seq, r->page0);
                    if (liv) ++plive[i]; else ++pdead[i];
                    alive_seq.push_back(liv ? 1 : 0);
                }
                const u32 n = (u32)alive_seq.size();
                if (!n) continue;
                u32 st = 0, mx = 0;
                for (u32 j = 0; j < n; ++j) {
                    if (!alive_seq[j]) {
                        ++st; if (st > mx) mx = st;
                        if (j < n / 3) ++d_lo[i];
                        else if (j < 2 * n / 3) ++d_mid[i];
                        else ++d_hi[i];
                    } else st = 0;
                }
                if (mx > streak_max[i]) streak_max[i] = mx;
            }
        }
        // 累加碎片统计（按 run 级汇总到函数外静态？放 row 扩展）
        row.dead_lo = row.dead_mid = row.dead_hi = 0;
        row.mix_pages_dense = row.mix_pages_sparse = 0;
        row.streak_sum = row.streak_n = 0;
        for (u32 i = 0; i < r->npages; ++i) {
            row.dead_lo += d_lo[i]; row.dead_mid += d_mid[i]; row.dead_hi += d_hi[i];
            if (pseen[i] && plive[i] && pdead[i]) {
                double df = (double)pdead[i] / (double)pseen[i];
                if (df >= 0.25) ++row.mix_pages_dense; else ++row.mix_pages_sparse;
                row.streak_sum += streak_max[i];
                ++row.streak_n;
                int b = df < 0.10 ? 0 : df < 0.25 ? 1 : df < 0.50 ? 2 : df < 0.75 ? 3 : 4;
                ++dens_hist[b];
            }
        }
        if (r->trim) {
            paint(r->page0, 1, ANCHOR);
            row.pg_anchor = 1;
        }
        for (u32 i = 0; i < r->npages; ++i) {
            if (r->trim && i > 0 && i < r->trim) continue;   // 已释放前缀
            if (r->trim && i == 0) continue;                 // 锚已画
            u8 k;
            if (pseen[i] == 0 && plive[i] == 0) {
                // 无段引用：物理仍属 run ⇒ 全死页（或空页）
                k = DEAD; ++row.pg_dead;
            } else if (plive[i] == 0) {
                k = DEAD; ++row.pg_dead;
            } else if (plive[i] >= pseen[i]) {
                k = LIVE; ++row.pg_live;
            } else {
                k = MIX; ++row.pg_mix;
            }
            paint(r->page0 + i, 1, k);
        }
        for (u32 i = 0; i < r->mnpg + r->aux_npg; ++i) ++row.pg_meta;
        rows.push_back(row);
    }
    u32 cnt[16] = {};
    for (u32 p = 2; p < tail; ++p) ++cnt[kind[p]];
    std::printf("== LIVE_INV %s tail=%u\n", tag, tail);
    std::printf("   pages: FREE=%u PEND=%u DEF=%u MAN=%u OVFL=%u META=%u ANCHOR=%u "
                "DATA_LIVE=%u DATA_MIX=%u DATA_DEAD=%u UNCLAIMED=%u\n",
                cnt[FREE], cnt[PEND], cnt[DEF], cnt[MAN], cnt[OVFL], cnt[META], cnt[ANCHOR],
                cnt[LIVE], cnt[MIX], cnt[DEAD], cnt[U]);
    std::printf("   records: nrec=%llu exact_live=%llu dead=%llu dead_frac=%.3f\n",
                (unsigned long long)rec_tot, (unsigned long long)rec_live,
                (unsigned long long)rec_dead,
                rec_tot ? (double)rec_dead / (double)rec_tot : 0.0);
    std::sort(rows.begin(), rows.end(), [](const RunRow& a, const RunRow& b) {
        u32 da = a.nrec > a.live_exact ? a.nrec - a.live_exact : 0;
        u32 db = b.nrec > b.live_exact ? b.nrec - b.live_exact : 0;
        return da > db;
    });
    std::printf("   top dirty runs (page0 lvl np nrec live_exact pgL/M/D/A trim):\n");
    for (u32 i = 0; i < rows.size() && i < 12; ++i) {
        const RunRow& r = rows[i];
        std::printf("     %u L%u np=%u nrec=%u live=%u pages L/M/D/A=%u/%u/%u/%u trim=%u\n",
                    r.page0, r.lvl, r.np, r.nrec, r.live_exact,
                    r.pg_live, r.pg_mix, r.pg_dead, r.pg_anchor, r.trim);
    }
    u32 data_all = cnt[LIVE] + cnt[MIX] + cnt[DEAD] + cnt[ANCHOR];
    std::printf("   reclaim_gate: DATA_DEAD+ANCHOR=%u (%.3f of data_pages=%u); "
                "MIX=%u (P2c-ish partial); FREE+PEND=%.3f of tail\n",
                cnt[DEAD] + cnt[ANCHOR],
                data_all ? (double)(cnt[DEAD] + cnt[ANCHOR]) / (double)data_all : 0.0,
                data_all, cnt[MIX],
                tail ? (double)(cnt[FREE] + cnt[PEND]) / (double)tail : 0.0);

    // --- 死碎片分布：层 / 密度 / 页内三分位 / 连续 streak ---
    u32 lvl_dead[8] = {}, lvl_nrec[8] = {}, lvl_mix[8] = {}, lvl_np[8] = {};
    u64 tri_lo = 0, tri_mid = 0, tri_hi = 0, st_sum = 0, st_n = 0;
    u32 mix_sparse = 0, mix_dense = 0;
    for (const RunRow& r : rows) {
        u32 lv = r.lvl < 8 ? r.lvl : 7;
        u32 dd = r.nrec > r.live_exact ? r.nrec - r.live_exact : 0;
        lvl_dead[lv] += dd; lvl_nrec[lv] += r.nrec; lvl_mix[lv] += r.pg_mix; lvl_np[lv] += r.np;
        tri_lo += r.dead_lo; tri_mid += r.dead_mid; tri_hi += r.dead_hi;
        mix_sparse += r.mix_pages_sparse; mix_dense += r.mix_pages_dense;
        st_sum += r.streak_sum; st_n += r.streak_n;
    }
    std::printf("   FRAG by_level (dead_recs/nrec mix_pages/np):\n");
    for (u32 lv = 0; lv < 4; ++lv) {
        if (!lvl_nrec[lv]) continue;
        std::printf("     L%u dead_frac=%.3f dead=%u/%u mix_pages=%u/%u\n",
                    lv, (double)lvl_dead[lv] / (double)lvl_nrec[lv],
                    lvl_dead[lv], lvl_nrec[lv], lvl_mix[lv], lvl_np[lv]);
    }
    u64 tri_all = tri_lo + tri_mid + tri_hi;
    std::printf("   FRAG dens_hist MIX pages dead_frac [<10%%]=%u [10-25)=%u [25-50)=%u [50-75)=%u [≥75]=%u\n",
                dens_hist[0], dens_hist[1], dens_hist[2], dens_hist[3], dens_hist[4]);
    std::printf("   FRAG in_page: dead_recs tertiles lo/mid/hi=%llu/%llu/%llu (%.0f/%.0f/%.0f%%)  "
                "MIX sparse(<25%%dead)=%u dense(≥25%%)=%u  avg_max_dead_streak=%.2f\n",
                (unsigned long long)tri_lo, (unsigned long long)tri_mid, (unsigned long long)tri_hi,
                tri_all ? 100.0 * tri_lo / tri_all : 0.0,
                tri_all ? 100.0 * tri_mid / tri_all : 0.0,
                tri_all ? 100.0 * tri_hi / tri_all : 0.0,
                mix_sparse, mix_dense,
                st_n ? (double)st_sum / (double)st_n : 0.0);
    std::printf("   FRAG mechanism_hint: tertile≈flat⇒随机覆盖写打散；prefix-heavy⇒TRIM边界/有序键；"
                "dense MIX⇒可议页内回收；sparse+PEND⇒峰主因非页内死字节\n");
}

int main(int argc, char** argv) {
    u32 N = argc > 1 ? (u32)atoi(argv[1]) : 200000;
    u32 B = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 VSZ = argc > 3 ? (u32)atoi(argv[3]) : 32;
    int quiet_small = argc > 4 ? atoi(argv[4]) : 1;
    int do_inv = argc > 5 ? atoi(argv[5]) : 1;
    int p1_stream = argc > 6 ? atoi(argv[6]) : 0;  // 坑 132/133：protect_stream_small
    int bf_mode = argc > 7 ? atoi(argv[7]) : 0;    // 坑 134/135：0关 1全量 2仅小np
    int bf_skip = argc > 8 ? atoi(argv[8]) : 0;    // 坑 135-C：近满旁路 pct
    int starve_fit = argc > 9 ? atoi(argv[9]) : -1;       // 坑 144：-1=继承 Soft P2；0/1=显式
    int starve_promote = argc > 10 ? atoi(argv[10]) : -1;
    u32 starve_floor = argc > 11 ? (u32)atoi(argv[11]) : 0;   // 坑 140；0=宏默认
    u32 max_slices = argc > 12 ? (u32)atoi(argv[12]) : 0;
    u32 max_prom = argc > 13 ? (u32)atoi(argv[13]) : 0;
    u32 prom_min_pend = argc > 14 ? (u32)atoi(argv[14]) : 0;
    int soft_cons = argc > 15 ? atoi(argv[15]) : -1;  // 坑 151：-1 继承 Soft；≥0 覆盖 SoftCons
    const char* P = "build/soft_a1.db";
    std::remove(P);
    FileDev dev(P, (PageId)((N * (VSZ + 24) + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) { std::printf("open fail\n"); return 1; }
    s.configure_deploy(false, 150);   // Soft：护洞+hole_fit+frag+hw+P2；space_accel / P1/bf 仍关
    if (p1_stream) s.set_protect_stream_small(true);
    if (bf_mode > 0) s.set_stream_best_fit_mode((u32)bf_mode);
    if (bf_skip > 0) s.set_stream_best_fit_skip_pct((u32)bf_skip);
    if (starve_fit >= 0) s.set_part_starve_fit(starve_fit != 0);
    if (starve_promote >= 0) s.set_starve_promote(starve_promote != 0);
    if (starve_floor) s.set_starve_floor(starve_floor);
    if (max_slices) s.set_starve_max_slices(max_slices);
    if (max_prom) s.set_starve_max_promotes(max_prom);
    // argc>14 时连 0 也生效（显式关 β 页数门槛；默认宏=64）
    if (argc > 14) s.set_starve_prom_min_pend(prom_min_pend);
    if (soft_cons >= 0) s.set_soft_cons((Store::SoftCons)std::min(soft_cons, 3));
    std::printf("# Soft A1: N=%u B=%u VSZ=%u protect=%d p1=%d bf_mode=%u bf_skip=%u "
                "starve_fit=%d starve_promote=%d floor=%u slices=%u max_prom=%u min_pend=%u "
                "soft_cons=%d hw=%u space_accel=%u frag=%d\n",
                N, B, VSZ, (int)s.protect_merge_holes(), (int)s.protect_stream_small(),
                s.stream_best_fit_mode(), s.stream_best_fit_skip_pct(),
                (int)s.part_starve_fit(), (int)s.starve_promote(),
                s.starve_floor(), s.starve_max_slices(), s.starve_max_promotes(),
                s.starve_prom_min_pend(), (int)s.soft_cons(),
                s.hw_reclaim_pct(), s.space_accel_pct(), (int)s.part_frag());

    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8) + 1;
    std::vector<u8> val(VSZ, 7), val2(VSZ, 9);

    u32 commits = 0, peak = 0, peak_at = 0;
    Snap at_peak{};
    u64 sum_delta = 0;
    u32 n_big = 0, n_hole_starve = 0, n_pre_dirty = 0, n_sb_lag = 0;
    // 峰前「合前基线」：第一次出现 L2≥lvl_t 且随后大涨前的 ratio
    double merge_start_ratio = 0;
    u32 merge_start_tail = 0, merge_start_live = 0;
    bool saw_merge_wave = false;

    auto emit = [&](const char* tag, Snap before, Snap after) {
        if (after.tail <= before.tail) return;
        u32 d = after.tail - before.tail;
        sum_delta += d;
        const bool big = d >= 64;
        // 大涨：补算存活账再估 dead（懒账会把 dead_est 吹到 0.4× 假阳性）
        if (big) {
            s.recount_all_unknown();
            after = snap(s);
            d = after.tail > before.tail ? after.tail - before.tail : d;
        }
        if (after.tail > peak) {
            peak = after.tail;
            peak_at = commits;
            at_peak = after;
            at_peak.peak = peak;
            if (do_inv && big) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "PEAK_c%u", peak_at);
                dump_live_inv(s, buf);
            }
        }
        u64 live = after.live ? after.live : 1;
        double r0 = (double)before.tail / (double)(before.live ? before.live : 1);
        double r1 = (double)after.tail / (double)live;
        const bool pre = r0 >= 1.30;
        const bool starve = after.max_hole < 64 && d >= 64;
        const bool lag = after.pend >= 64 && after.max_hole < (u32)std::max<u64>(64ull, (u64)d / 2);
        if (big) {
            ++n_big;
            if (pre) ++n_pre_dirty;
            if (starve) ++n_hole_starve;
            if (lag) ++n_sb_lag;
            if (!saw_merge_wave && after.L2 + after.L3 >= 8 && d >= 100) {
                saw_merge_wave = true;
                merge_start_ratio = r0;
                merge_start_tail = before.tail;
                merge_start_live = (u32)before.live;
            }
        }
        if (quiet_small && !big) return;
        const char* kind = starve ? "HOLE_STARVE" : (lag ? "SB_LAG" : (pre ? "PRE_DIRTY" : "OTHER"));
        std::printf("%s#%u tail %u->%u (+%u) r0=%.3f r1=%.3f holes=%u/%llu maxh=%u pend=%llu "
                    "L0=%u L1=%u L2=%u L3=%u dead_est=%llu owned=%llu ATTR=%s\n",
                    tag, commits, before.tail, after.tail, d, r0, r1,
                    after.holes, (unsigned long long)after.free_tot, after.max_hole,
                    (unsigned long long)after.pend, after.L0, after.L1, after.L2, after.L3,
                    (unsigned long long)after.dead_est, (unsigned long long)after.owned, kind);
    };

    Snap prev = snap(s);
    const double t_ins0 = prog_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) {
            s.commit();
            ++commits;
            Snap cur = snap(s);
            emit("ins ", prev, cur);
            prev = cur;
        }
    }
    s.commit(); ++commits;
    { Snap cur = snap(s); emit("ins ", prev, cur); prev = cur; }
    const double t_ins1 = prog_us();
    std::printf("== after insert: tail=%u live=%llu peak=%u runs=%u\n",
                s.tail_, (unsigned long long)s.live_pages(), peak, s.run_count());
    std::printf("== PHASE_US insert_total=%.0f insert_us_op=%.3f\n",
                t_ins1 - t_ins0, (t_ins1 - t_ins0) / (double)N);

    const double t_upd0 = prog_us();
    for (u32 i = 0; i < N / 2; ++i) {
        u64 k = keys[mix(i + 999) % N];
        s.put(k, val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) {
            s.commit();
            ++commits;
            Snap cur = snap(s);
            emit("upd ", prev, cur);
            prev = cur;
        }
    }
    s.commit(); ++commits;
    { Snap cur = snap(s); emit("upd ", prev, cur); prev = cur; }
    const double t_upd1 = prog_us();
    std::printf("== PHASE_US update_total=%.0f update_us_op=%.3f\n",
                t_upd1 - t_upd0, (t_upd1 - t_upd0) / (double)(N / 2));

    { double w0 = prog_us();
      while (s.maint_pending() && prog_us() - w0 < 60e6) s.commit();
      s.commit(); s.commit(); }
    { Snap cur = snap(s); emit("fin ", prev, cur); }

    u64 live_logic = s.live_pages() ? s.live_pages() : 1;          // 与 layout / harness 同口径
    u64 live_phys = s.live_page_count() ? s.live_page_count() : 1;
    double peak_x = (double)peak / (double)live_logic;
    double end_x = (double)s.tail_ / (double)live_logic;
    double dead_x = (double)at_peak.dead_est / (double)live_logic;
    double pre_x = merge_start_live ? (double)merge_start_tail / (double)merge_start_live : merge_start_ratio;
    double residual = peak_x - (pre_x > 1.0 ? pre_x : 1.0);
    if (residual < 0) residual = 0;

    auto ms = s.maint_stats();
    std::printf("== SUMMARY Soft A1 peak_tail=%u peak_x=%.3f end_tail=%u end_x=%.3f live_logic=%llu live_phys=%llu\n",
                peak, peak_x, s.tail_, end_x, (unsigned long long)live_logic, (unsigned long long)live_phys);
    std::printf("== ATTR_COUNTS big_grows=%u PRE_DIRTY=%u HOLE_STARVE=%u SB_LAG=%u sum_delta=%llu\n",
                n_big, n_pre_dirty, n_hole_starve, n_sb_lag, (unsigned long long)sum_delta);
    std::printf("== MERGE_START tail=%u live=%u ratio=%.3f (first big L2/L3 wave)\n",
                merge_start_tail, merge_start_live, pre_x);
    std::printf("== AT_PEAK commit=%u dead_est=%llu owned=%llu dead/live=%.3f maxh=%u pend=%llu "
                "L0=%u L1=%u L2=%u L3=%u\n",
                peak_at, (unsigned long long)at_peak.dead_est, (unsigned long long)at_peak.owned, dead_x,
                at_peak.max_hole, (unsigned long long)at_peak.pend,
                at_peak.L0, at_peak.L1, at_peak.L2, at_peak.L3);
    std::printf("== DECOMP leading: pre_dirty≈%.3f + residual≈%.3f (peak_x)  "
                "P2c_gate dead/live=%.3f %s\n",
                pre_x, residual, dead_x,
                dead_x < 0.10 ? "→勿开P2c(死页不足)" : "→可议P2c");
    std::printf("== maint hole_fits=%llu hw_reclaims=%llu part_frags=%llu stream_extends=%llu "
                "hole_protect_skips=%llu p1=%d stream_best_fits=%llu bf_mode=%u bf_skip=%u "
                "starve_fits=%llu starve_promotes=%llu starve_prom_ms=%.2f\n",
                (unsigned long long)s.part_hole_fits(), (unsigned long long)ms.hw_reclaims,
                (unsigned long long)s.part_frags(), (unsigned long long)ms.stream_extends,
                (unsigned long long)s.hole_protect_skips(), (int)s.protect_stream_small(),
                (unsigned long long)s.stream_best_fits(), s.stream_best_fit_mode(),
                s.stream_best_fit_skip_pct(),
                (unsigned long long)s.part_starve_fits(), (unsigned long long)s.starve_promotes(),
                s.starve_prom_us() / 1000.0);
    if (do_inv) dump_live_inv(s, "END");
    std::remove(P);
    return 0;
}
