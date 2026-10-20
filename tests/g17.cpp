// g17 —— 第 17 轮回归 ①：fsync 出锁（提交分段 + 提交顺序器 + 两个 SB 代数 + durable_vers()）。
//   (1) t_basic：单线程语义不变——commit() 返回后 durable_vers() == last_commit()；commit(false) 只前移 last_commit()；
//       sb_built_ == sb_synced_；重开比对 + verify。
//   (2) t_lockfree_sync：慢 sync 设备（每次 do_sync 睡 40 ms）。写者线程 commit() 期间（设备正睡在 sync 里）主线程 get：
//       新值必须**已可见**、延迟必须**远小于** sync 时长（锁外 fsync 的判据；老头这里会被锁住 ≥ 80 ms），且此刻
//       last_commit() > durable_vers()；写者返回后两者相等。
//   (3) t_empty_ticket：A 在飞（睡在 sync 里）时 B 无新写调 commit() ⇒ B 必须等 A 持久后才返回（返回时 durable_vers() ≥ A 的版本）。
//   (4) t_phase2_fail：SB 写失败注入。单线程：commit() 报 IO、账脏、下一次调用按盘重建、再提交 OK、重开一致。
//       并发：A 睡在 sync 里时 B 发了票，A 的 SB 写失败 ⇒ B 也必须报 IO（粘性 cfail_），之后恢复、重开一致。
//   (5) t_snapshot_inflight：A 在飞时 snapshot()（内联路径先排空在飞票）⇒ 返回后 durable_vers() ≥ 快照版本；rollback 正常。
//   (6) t_multi_writers：3 写者 × 2 读者 × mode 3 后台维护：每个写者自己的键域，durable 提交与 commit(false) 混合；读者校验
//       durable_vers() 单调且 ≤ last_commit()；收尾 cseq_ == cdone_、sb_built_ == sb_synced_、全部键在、verify、重开比对。
//   (7) t_wide_family（第 17 轮 ③ 家族虚拟源游标）：小片（set_merge_part_pages(16)）× lvl_t 4 ⇒ L2→L3 归并的源 = 4 个家族 × 十几片
//   (8) t_hole_fit（第 17 轮 ②(c3) 中等产物按洞分片）：layout 200k 同款负载、无时间预算 × 每提交 8 单元（确定性）；开 ⇒ 触发 ≥1 且峰值 ≤ 关；关 ⇒ 0 次
//       （源 run 数 > 64）。老头的源下标 / 可见位掩码限 64 条 ⇒ 存活账漂移（famw 探针：verify "live mismatch" 851 处）。
//       判据：merge_src_max > 64 且 merge_cur_max ≤ 家族数、verify 零错、账零漂移（逐单元 recount 对账）、与真值比对、重开比对。
// 判据：每段 verify + 与真值比对；失败打印第一处。单跑 ≈ 8 s（RamDev）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#define private public
#include "fcs.h"
#undef private
using namespace fcs;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 可注入的设备：慢 sync（睡 sync_delay_ms）、SB 槽（页 0/1）写失败 fail_sb 次
struct HookDev : RamDev {
    std::atomic<int> sync_delay_ms{0};
    std::atomic<int> in_sync{0};
    std::atomic<unsigned> syncs{0};
    std::atomic<int> fail_sb{0};
    std::atomic<unsigned> sb_writes{0};
    using RamDev::RamDev;
    void do_sync() override {
        syncs.fetch_add(1);
        in_sync.fetch_add(1);
        int d = sync_delay_ms.load();
        if (d) sleep_ms(d);
        in_sync.fetch_sub(1);
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (pid < 2) {
            sb_writes.fetch_add(1);
            int f = fail_sb.load();
            while (f > 0) { if (fail_sb.compare_exchange_weak(f, f - 1)) return RC::IO; }
        }
        return RamDev::do_write(pid, off, src, len);
    }
    bool wait_in_sync(double tmo = 3.0) {
        double t0 = now_s();
        while (in_sync.load() == 0 && now_s() - t0 < tmo) sleep_ms(1);
        return in_sync.load() > 0;
    }
};

struct Model { std::map<u64, u32> m; };
void putm(Store& s, Model& t, u64 k, u32 tag, u32 len = 120) {
    std::vector<u8> v(len, (u8)(tag * 7 + 1)); std::memcpy(v.data(), &tag, 4);
    s.put(k, v.data(), (u16)len); t.m[k] = tag;
}
int cmp_model(Store& s, const Model& t, const char* tag) {
    static u8 buf[65536]; u16 len = 0; int bad = 0;
    for (auto& kv : t.m) {
        RC rc = s.get(kv.first, buf, 65535, &len);
        bool ok = kv.second == 0 ? rc == RC::NOTFOUND : (rc == RC::OK && len >= 4 && std::memcmp(buf, &kv.second, 4) == 0);
        if (!ok && bad++ < 3) std::printf("  %s FAIL: key %llu rc=%s len=%u want tag %u\n", tag, (unsigned long long)kv.first, rc_str(rc), len, kv.second);
    }
    return bad ? 1 : 0;
}
int vok(Store& s, const char* tag) {
    Report rep; RC rc = s.verify(&rep);
    if (rc != RC::OK || rep.errors) { std::printf("  %s FAIL: verify rc=%s errors=%u first=%s key=%llu\n", tag, rc_str(rc), rep.errors, rep.first_err, (unsigned long long)rep.first_key); return 1; }
    return 0;
}
int chk(bool c, const char* what, int& bad) { if (!c) { std::printf("  FAIL: %s\n", what); ++bad; } return bad; }

// ---------------------------------------------------------------- (1)
int t_basic() {
    RamDev dev(4096); u8* arena = new u8[1024 * PAGE]; int bad = 0; Model md;
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_basic open fail\n"); return 1; }
        chk(s.durable_vers() == s.last_commit(), "open: durable_vers == last_commit", bad);
        for (int r = 0; r < 20; ++r) {
            for (int i = 0; i < 300; ++i) putm(s, md, (u64)(r * 300 + i) * 7 + 1, (u32)(r * 1000 + i + 1));
            if (r % 4 == 3) {
                chk(s.commit(false) == RC::OK, "commit(false)", bad);
                chk(s.last_commit() > s.durable_vers(), "commit(false): last_commit > durable_vers", bad);
            } else {
                chk(s.commit() == RC::OK, "commit", bad);
                chk(s.durable_vers() == s.last_commit(), "commit: durable_vers == last_commit", bad);
                chk(s.sb_built_ == s.sb_synced_, "single thread: sb_built_ == sb_synced_", bad);
                chk(s.cseq_ == s.cdone_.load(), "single thread: cseq_ == cdone_", bad);
            }
        }
        chk(s.commit() == RC::OK, "final commit", bad);
        chk(s.durable_vers() == s.last_commit(), "final: durable_vers == last_commit", bad);
        bad += cmp_model(s, md, "t_basic") + vok(s, "t_basic");
    }
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_basic reopen fail\n"); return bad + 1; }
        chk(s.durable_vers() == s.last_commit(), "reopen: durable_vers == last_commit", bad);
        bad += cmp_model(s, md, "t_basic reopen") + vok(s, "t_basic reopen");
    }
    delete[] arena;
    std::printf("  t_basic: durable_vers 语义 / 代数 / 重开  %s\n", bad ? "FAIL" : "ok");
    return bad;
}

// ---------------------------------------------------------------- (2)
int t_lockfree_sync(int rounds) {
    HookDev dev(4096); u8* arena = new u8[1024 * PAGE]; int bad = 0; Model md;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_lockfree open fail\n"); return 1; }
    s.set_maint_mode(1);
    for (int i = 0; i < 2000; ++i) putm(s, md, (u64)i * 3 + 1, (u32)i + 1);
    chk(s.commit() == RC::OK, "warm commit", bad);
    double get_max_ms = 0, commit_max_ms = 0; int seen_window = 0;
    dev.sync_delay_ms = 40;
    for (int r = 0; r < rounds; ++r) {
        u64 k = 1000003ull + (u64)r; u32 tag = 777000u + (u32)r;
        std::vector<u8> v(120, 1); std::memcpy(v.data(), &tag, 4);
        std::atomic<int> state{0}; RC wrc = RC::OK; double tc0 = 0, tc1 = 0;
        std::thread w([&] { s.put(k, v.data(), 120); state = 1; tc0 = now_s(); wrc = s.commit(); tc1 = now_s(); state = 2; });
        while (state.load() == 0) sleep_ms(1);
        bool in = dev.wait_in_sync();              // 写者正睡在阶段二的 sync 里（锁已放）
        if (in && state.load() == 1) {
            ++seen_window;
            u64 lc = s.last_commit(), dv = s.durable_vers();
            chk(lc > dv, "window: last_commit > durable_vers", bad);
            u8 buf[256]; u16 len = 0; double t0 = now_s();
            RC rc = s.get(k, buf, 255, &len);
            double dt = (now_s() - t0) * 1000.0; get_max_ms = std::max(get_max_ms, dt);
            chk(rc == RC::OK && len == 120 && std::memcmp(buf, &tag, 4) == 0, "window: new value visible", bad);
            // 判据：get 不被提交者的 fsync 挡住。老头（锁内 sync→SB→sync）这里 ≥ 80 ms；新头只剩 µs 级（留 20 ms 噪声余量）。
            if (dt > 20.0) { std::printf("  FAIL: get during fsync took %.1f ms (round %d)\n", dt, r); ++bad; }
        }
        w.join();
        md.m[k] = tag;
        chk(wrc == RC::OK, "writer commit rc", bad);
        commit_max_ms = std::max(commit_max_ms, (tc1 - tc0) * 1000.0);
        chk(s.durable_vers() == s.last_commit(), "after writer: durable_vers == last_commit", bad);
    }
    dev.sync_delay_ms = 0;
    chk(seen_window >= rounds / 2, "caught the sync window in at least half the rounds", bad);
    bad += cmp_model(s, md, "t_lockfree") + vok(s, "t_lockfree");
    delete[] arena;
    std::printf("  t_lockfree_sync: %d/%d 轮抓到 sync 窗口；窗口内 get max %.2f ms（sync 睡 40 ms×2）；提交 max %.0f ms  %s\n",
                seen_window, rounds, get_max_ms, commit_max_ms, bad ? "FAIL" : "ok");
    return bad;
}

// ---------------------------------------------------------------- (3)
int t_empty_ticket() {
    HookDev dev(4096); u8* arena = new u8[1024 * PAGE]; int bad = 0; Model md;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_empty open fail\n"); return 1; }
    for (int i = 0; i < 500; ++i) putm(s, md, (u64)i * 5 + 2, (u32)i + 1);
    chk(s.commit() == RC::OK, "warm commit", bad);
    dev.sync_delay_ms = 40;
    int caught = 0; double b_min_ms = 1e9;
    for (int r = 0; r < 4; ++r) {
        putm(s, md, 5000000ull + (u64)r, 4242u + (u32)r);
        std::atomic<int> st{0}; RC arc = RC::OK, brc = RC::OK;
        std::thread A([&] { arc = s.commit(); st = 2; });
        if (dev.wait_in_sync() && st.load() == 0) {
            ++caught;
            u64 vA = s.last_commit(); u64 dv0 = s.durable_vers();
            chk(dv0 < vA, "A in flight: durable_vers < last_commit", bad);
            double tb0 = now_s(); brc = s.commit();     // 无新写：空票，必须等 A 持久
            double dtb = (now_s() - tb0) * 1000.0; b_min_ms = std::min(b_min_ms, dtb);
            chk(brc == RC::OK, "B rc", bad);
            chk(s.durable_vers() >= vA, "B returned ⇒ A's version durable", bad);
        }
        A.join();
        chk(arc == RC::OK, "A rc", bad);
    }
    dev.sync_delay_ms = 0;
    chk(caught >= 2, "caught A in flight at least twice", bad);
    bad += cmp_model(s, md, "t_empty") + vok(s, "t_empty");
    delete[] arena;
    std::printf("  t_empty_ticket: 抓到 %d/4 次；B（无新写）最短等待 %.0f ms  %s\n", caught, b_min_ms < 1e8 ? b_min_ms : 0.0, bad ? "FAIL" : "ok");
    return bad;
}

// ---------------------------------------------------------------- (4)
int t_phase2_fail() {
    HookDev dev(4096); u8* arena = new u8[1024 * PAGE]; int bad = 0; Model md;
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_fail open fail\n"); return 1; }
        for (int i = 0; i < 400; ++i) putm(s, md, (u64)i * 11 + 3, (u32)i + 1);
        chk(s.commit() == RC::OK, "warm commit", bad);
        // 单线程：SB 写失败 ⇒ IO、账脏；下一次调用重建；再提交 OK
        u64 dv_before = s.durable_vers();
        putm(s, md, 9000001ull, 31337u);
        dev.fail_sb = 1;
        RC rc = s.commit();
        chk(rc == RC::IO, "SB write failure ⇒ commit IO", bad);
        chk(s.dirty_.load(), "after failure: dirty_", bad);
        chk(s.durable_vers() == dv_before, "after failure: durable_vers unchanged", bad);
        dev.fail_sb = 0;
        u8 buf[256]; u16 len = 0;
        rc = s.get(9000001ull, buf, 255, &len);                 // ensure_ready ⇒ reload（内存链含失败那次的项）
        chk(rc == RC::OK && len == 120, "after reload: value still readable", bad);
        chk(!s.dirty_.load(), "after reload: clean", bad);
        chk(s.commit() == RC::OK, "re-commit OK", bad);
        chk(s.durable_vers() == s.last_commit(), "re-commit: durable", bad);
        bad += cmp_model(s, md, "t_fail single") + vok(s, "t_fail single");

        // 并发：A 睡在 sync 里时 B 发票；A 的 SB 写失败 ⇒ B 也 IO（粘性）
        dev.sync_delay_ms = 40;
        putm(s, md, 9000002ull, 31338u);
        std::atomic<int> st{0}; RC arc = RC::OK, brc = RC::OK;
        dev.fail_sb = 1;
        std::thread A([&] { arc = s.commit(); st = 2; });
        bool caught = dev.wait_in_sync() && st.load() == 0;
        if (caught) {
            putm(s, md, 9000003ull, 31339u);
            brc = s.commit();                                   // 阶段一成功、阶段二排在 A 后面 ⇒ A 失败 ⇒ B IO
        }
        A.join();
        dev.sync_delay_ms = 0; dev.fail_sb = 0;
        chk(arc == RC::IO, "concurrent: A IO", bad);
        if (caught) chk(brc == RC::IO, "concurrent: B IO (sticky cfail_)", bad);
        else std::printf("  (并发失败段没抓到窗口，跳过 B 判据)\n");
        chk(s.commit() == RC::OK, "recover commit OK", bad);
        chk(!s.cfail_.load(), "cfail_ reset after reload", bad);
        chk(s.durable_vers() == s.last_commit(), "recovered: durable", bad);
        bad += cmp_model(s, md, "t_fail concurrent") + vok(s, "t_fail concurrent");
    }
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_fail reopen fail\n"); return bad + 1; }
        bad += cmp_model(s, md, "t_fail reopen") + vok(s, "t_fail reopen");
    }
    delete[] arena;
    std::printf("  t_phase2_fail: 单线程 IO→重建→恢复；并发粘性失败；重开一致  %s\n", bad ? "FAIL" : "ok");
    return bad;
}

// ---------------------------------------------------------------- (5)
int t_snapshot_inflight() {
    HookDev dev(4096); u8* arena = new u8[1024 * PAGE]; int bad = 0; Model md;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_snap open fail\n"); return 1; }
    for (int i = 0; i < 500; ++i) putm(s, md, (u64)i * 13 + 4, (u32)i + 1);
    chk(s.commit() == RC::OK, "warm commit", bad);
    dev.sync_delay_ms = 40;
    putm(s, md, 7000001ull, 555u);
    std::atomic<int> st{0}; RC arc = RC::OK;
    std::thread A([&] { arc = s.commit(); st = 2; });
    bool caught = dev.wait_in_sync() && st.load() == 0;
    u32 snap_id = 0; RC src = RC::OK;
    if (caught) {
        src = s.snapshot(); snap_id = s.nsnap_ - 1;              // 内联路径：先排空在飞的 A，再抓
        chk(src == RC::OK, "snapshot rc", bad);
        chk(s.durable_vers() >= s.snap_version(snap_id), "snapshot ⇒ its version durable", bad);
    }
    A.join();
    dev.sync_delay_ms = 0;
    chk(arc == RC::OK, "A rc", bad);
    if (!caught) { src = s.snapshot(); snap_id = s.nsnap_ - 1; chk(src == RC::OK, "snapshot rc (late)", bad); std::printf("  (快照段没抓到窗口)\n"); }
    Model after = md;
    for (int i = 0; i < 200; ++i) putm(s, after, (u64)i * 13 + 4, 90000u + (u32)i);
    chk(s.commit() == RC::OK, "commit after snapshot", bad);
    bad += cmp_model(s, after, "t_snap after");
    chk(s.rollback(snap_id) == RC::OK, "rollback", bad);
    bad += cmp_model(s, md, "t_snap rolled back") + vok(s, "t_snap");
    chk(s.durable_vers() == s.last_commit(), "rollback ⇒ durable", bad);
    delete[] arena;
    std::printf("  t_snapshot_inflight: 在飞时 snapshot()（抓到 %d）/ rollback  %s\n", (int)caught, bad ? "FAIL" : "ok");
    return bad;
}

// ---------------------------------------------------------------- (6)
int t_multi_writers(int seed, int iters) {
    RamDev dev(16384); u8* arena = new u8[2048 * PAGE]; int bad = 0;
    const int NW = 3, NR = 2;
    std::vector<Model> md(NW);
    {
        Store s; if (s.open(&dev, arena, 2048) != RC::OK) { std::printf("  t_multi open fail\n"); return 1; }
        s.set_maint_mode(3); s.set_lvl_t(4); s.set_merge_budget_pages(32);
        std::atomic<bool> stop{false}; std::atomic<int> wbad{0}, rbad{0}; std::atomic<long> gets{0}; std::atomic<int> lcommits{0}, dcommits{0};
        std::vector<std::thread> th;
        for (int w = 0; w < NW; ++w) th.emplace_back([&, w] {
            std::mt19937_64 rng(seed * 100 + w);
            for (int it = 0; it < iters; ++it) {
                for (int i = 0; i < 20; ++i) {
                    u64 k = (u64)w * 1000000ull + (rng() % 4000) + 1; u32 tag = (u32)(it * 100 + i + 1);
                    std::vector<u8> v(100, (u8)tag); std::memcpy(v.data(), &tag, 4);
                    if (s.put(k, v.data(), 100) != RC::OK) { ++wbad; continue; }
                    md[w].m[k] = tag;
                }
                RC rc;
                if (it % 5 == 4) { rc = s.commit(false); ++lcommits; } else { rc = s.commit(); ++dcommits; }
                if (rc != RC::OK) { std::printf("  writer %d commit rc=%s\n", w, rc_str(rc)); ++wbad; }
                // 注：这里不比较 durable_vers() 与 last_commit()——last_commit() 是无锁裸读 vers_（老接口），别的写者正在改它（TSan 会报）。
            }
            s.commit();
        });
        for (int r = 0; r < NR; ++r) th.emplace_back([&, r] {
            std::mt19937_64 rng(seed * 100 + 50 + r); u64 dv_prev = 0; u8 buf[256]; u16 len;
            while (!stop.load()) {
                u64 dv = s.durable_vers();               // 只查单调（与 last_commit() 的比较无法原子地做）
                if (dv < dv_prev) ++rbad;
                dv_prev = dv;
                int w = (int)(rng() % NW); u64 k = (u64)w * 1000000ull + (rng() % 4000) + 1;
                RC rc = s.get(k, buf, 255, &len);
                if (rc != RC::OK && rc != RC::NOTFOUND) ++rbad;
                else if (rc == RC::OK && (len != 100 || buf[4] != (u8)*(u32*)buf)) ++rbad;
                ++gets;
            }
        });
        for (int w = 0; w < NW; ++w) th[w].join();
        stop = true;
        for (int r = 0; r < NR; ++r) th[NW + r].join();
        chk(wbad.load() == 0, "writer errors", bad);
        chk(rbad.load() == 0, "reader errors (durable_vers monotonic / get sanity)", bad);
        chk(s.commit() == RC::OK, "final commit", bad);
        chk(s.cseq_ == s.cdone_.load(), "cseq_ == cdone_", bad);
        chk(s.sb_built_ == s.sb_synced_, "sb_built_ == sb_synced_", bad);
        chk(s.durable_vers() == s.last_commit(), "durable == last", bad);
        for (int w = 0; w < NW; ++w) bad += cmp_model(s, md[w], "t_multi");
        bad += vok(s, "t_multi");
        std::printf("  t_multi_writers seed %d: %d 写者 × %d 轮（durable %d / logical %d）+ %d 读者 %ld 次 get，runs=%u  %s\n",
                    seed, NW, iters, dcommits.load(), lcommits.load(), NR, gets.load(), s.run_count(), bad ? "FAIL" : "ok");
    }
    {
        Store s; if (s.open(&dev, arena, 2048) != RC::OK) { std::printf("  t_multi reopen fail\n"); return bad + 1; }
        for (int w = 0; w < NW; ++w) bad += cmp_model(s, md[w], "t_multi reopen");
        bad += vok(s, "t_multi reopen");
    }
    delete[] arena;
    return bad;
}

// ---------------------------------------------------------------- (7)
int acct_drift(Store& s, const char* tag) {
    int bad = 0;
    if (s.job_) { std::printf("  %s: 作业未结束，跳过对账\n", tag); return 0; }
    for (auto& kv : s.cells_) {
        Cell* c = kv.second;
        if (!c->acct) continue;
        const u64 l0 = c->live, d0 = c->dead;
        std::vector<u32> sl; for (auto& sg : c->segs) sl.push_back(sg.second.live);
        s.recount(c);
        std::vector<u32> sl2; for (auto& sg : c->segs) sl2.push_back(sg.second.live);
        if (l0 != c->live || d0 != c->dead || sl != sl2) {
            if (bad++ < 3) std::printf("  %s FAIL: 账漂移 cell=[%llu,%llu) live %llu vs %llu dead %llu vs %llu\n", tag,
                                       (unsigned long long)c->lo, (unsigned long long)c->hi, (unsigned long long)l0, (unsigned long long)c->live,
                                       (unsigned long long)d0, (unsigned long long)c->dead);
        }
    }
    return bad ? 1 : 0;
}
static u64 mixk(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
int t_wide_family() {
    const u32 N = 200000, B = 500;
    RamDev dev(65536); u8* arena = new u8[4096 * PAGE]; int bad = 0; Model md;
    {
        Store s; if (s.open(&dev, arena, 4096) != RC::OK) { std::printf("  t_wide open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_lvl_t(4); s.set_merge_part_pages(16); s.set_merge_budget_pages(64); s.set_maint_budget_us(0);
        for (u32 i = 0; i < N; ++i) { putm(s, md, mixk(i + 1) % (N * 8) + 1, i + 1, 32); if ((i + 1) % B == 0) s.commit(); }
        for (u32 i = 0; i < N / 2; ++i) { putm(s, md, mixk(mixk(i + 999) % N + 1) % (N * 8) + 1, 500000u + i, 32); if ((i + 1) % B == 0) s.commit(); }
        s.commit();
        double t0 = now_s(); while (s.maint_pending() && now_s() - t0 < 120) s.commit();
        auto ms = s.maint_stats();
        chk(ms.merge_src_max > 64, "merge_src_max > 64 (源 run 数超过老头的 64 位掩码)", bad);
        chk(ms.merge_cur_max <= 8 * 4, "merge_cur_max ≤ 家族数上限 (cliff·t)", bad);
        chk(ms.merge_cur_max < ms.merge_src_max, "虚拟源：游标数 < 源 run 数", bad);
        bad += vok(s, "t_wide") + acct_drift(s, "t_wide") + cmp_model(s, md, "t_wide");
        std::printf("  t_wide_family: runs=%u src_max=%llu cur_max=%llu slices=%llu  %s\n", s.run_count(), (unsigned long long)ms.merge_src_max,
                    (unsigned long long)ms.merge_cur_max, (unsigned long long)ms.slices, bad ? "FAIL" : "ok");
    }
    {
        Store s; if (s.open(&dev, arena, 4096) != RC::OK) { std::printf("  t_wide reopen fail\n"); return bad + 1; }
        bad += vok(s, "t_wide reopen") + cmp_model(s, md, "t_wide reopen");
    }
    delete[] arena;
    return bad;
}

// (8) 第 17 轮 ②(c3)：中等产物按洞分片。layout 同款负载（随机键 200k × 32 B，B=500，更新一半），维护无时间预算（确定性布局）；
//     开 / 关 set_part_hole_fit 各跑一遍：开 ⇒ 至少触发一次、峰值 ≤ 关、verify / 账 / 真值 / 重开全对；关 ⇒ 触发计数为 0。
struct HfRes { u32 peak = 0, tail = 0; u64 live = 0, fits = 0; u32 runs = 0; };
int hole_fit_once(bool on, HfRes& r, Model& md, RamDev& dev, u8* arena, const char* tag) {
    const u32 N = 200000, B = 500; int bad = 0; md.m.clear();
    Store s; if (s.open(&dev, arena, 2048) != RC::OK) { std::printf("  t_hole_fit open fail\n"); return 1; }
    // 无时间预算（确定性）但每次提交最多 8 个维护单元：让归并跨多个提交点，裁掉的源前缀才能过提交点变成洞（budget 0 + 默认 256 单元
    // ⇒ 整个 L2→L3 归并在一个提交内做完，释放页全卡在 pending_free，产物 1584 页整体落尾，峰值 2.32×，与 layout 默认（20 ms 预算）不是一回事）
    s.set_maint_mode(1); s.set_maint_budget_us(0); s.set_maint_units_max(8); s.set_part_hole_fit(on);
    auto track = [&] { if (s.tail_ > r.peak) r.peak = s.tail_; };
    for (u32 i = 0; i < N; ++i) { putm(s, md, mixk(i + 1) % (N * 8) + 1, i + 1, 32); if ((i + 1) % B == 0) { s.commit(); track(); } }
    for (u32 i = 0; i < N / 2; ++i) { putm(s, md, mixk(mixk(i + 999) % N + 1) % (N * 8) + 1, 500000u + i, 32); if ((i + 1) % B == 0) { s.commit(); track(); } }
    s.commit(); track();
    double t0 = now_s(); while (s.maint_pending() && now_s() - t0 < 120) { s.commit(); track(); }
    s.commit(); s.commit(); track();
    r.tail = s.tail_; r.live = s.live_pages(); r.fits = s.part_hole_fits(); r.runs = s.run_count();
    bad += vok(s, tag) + acct_drift(s, tag) + cmp_model(s, md, tag);
    return bad;
}
int t_hole_fit() {
    int bad = 0; Model md; u8* arena = new u8[2048 * PAGE];
    HfRes on, off;
    { RamDev dev(65536); bad += hole_fit_once(true, on, md, dev, arena, "t_hole_fit on");
      { Store s; if (s.open(&dev, arena, 2048) != RC::OK) { std::printf("  t_hole_fit reopen fail\n"); bad++; } else bad += vok(s, "t_hole_fit reopen") + cmp_model(s, md, "t_hole_fit reopen"); } }
    { RamDev dev(65536); bad += hole_fit_once(false, off, md, dev, arena, "t_hole_fit off"); }
    chk(on.fits >= 1, "hole-fit 至少触发一次（layout 200k 负载）", bad);
    chk(off.fits == 0, "关掉开关 ⇒ 不触发", bad);
    chk(on.peak <= off.peak, "开 ⇒ 峰值 ≤ 关", bad);
    std::printf("  t_hole_fit: on  peak=%u (%.2fx) tail=%u runs=%u fits=%llu | off peak=%u (%.2fx) tail=%u runs=%u fits=%llu  %s\n",
                on.peak, (double)on.peak / std::max<u64>(1, on.live), on.tail, on.runs, (unsigned long long)on.fits,
                off.peak, (double)off.peak / std::max<u64>(1, off.live), off.tail, off.runs, (unsigned long long)off.fits, bad ? "FAIL" : "ok");
    delete[] arena;
    return bad;
}

} // namespace

int main(int argc, char** argv) {
    int iters = argc > 1 ? std::atoi(argv[1]) : 150;
    double t0 = now_s(); int bad = 0;
    bad += t_basic();
    bad += t_lockfree_sync(6);
    bad += t_empty_ticket();
    bad += t_phase2_fail();
    bad += t_snapshot_inflight();
    bad += t_multi_writers(1, iters);
    bad += t_multi_writers(2, iters);
    bad += t_wide_family();
    bad += t_hole_fit();
    std::printf("g17 第 17 轮：fsync 出锁（提交分段 / 顺序器 / 两个代数 / durable_vers）+ 家族虚拟源游标 + 中等产物按洞分片: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
