// g6 —— 掉电注入（G1，"易失写缓存 + 随机掉电"的模型检查）
//
// g5 只检查"重开后 == 上一个提交点"，但 RamDev 的写是同步、有序、永不丢失的 —— 它根本不会暴露
// "提交点写了但没落到介质"这一类问题。真实的文件/块设备有页缓存：pwrite 返回 ≠ 落盘，回写顺序任意，
// 掉电时自上次 fdatasync 以来的脏页**各自独立**地到达或丢失，正在写的那一页还可能只到一半。
// 这里用 VolDev 把这些都模拟出来：
//   * stable_ = 介质内容；RamDev 自己的缓冲 = 页缓存（引擎活着时读到的）
//   * do_write 只标脏；do_sync 才把脏页搬进 stable_
//   * 第 cut_at 次写时"掉电"：每个脏页 50% 到达 / 50% 丢失；当前这一笔 1/3 全到、1/3 全丢、1/3 撕裂（前一半到）
//   * 掉电后写照样返回 OK（进程并不知道设备已死），但再也进不了 stable_
//   * reboot：RamDev 缓冲 <- stable_，再开一个 Store
// 模型（沿用 g5）：truth / durable / 快照。掉电发生在第 i 步内 ⇒ 重开后的状态必须**恰好**是
//   { 第 i 步之前的 durable } ∪ { 第 i 步自己的提交点（仅当它是 commit/snapshot/rollback/维护 这类提交点）} 之一；
// 然后 verify()（含页所有权）+ scrub() 必须干净。之后以匹配到的那个状态为新的 truth/durable 继续跑。
// 第一轮抓到 4 个洞（见 DESIGN3 §12 坑 81–84；都是 g5 在"同步、有序、不丢"的 RamDev 上永远看不见的）：
//   坑 81：write_sb 之前没有持久化屏障 —— 数据页 → SB → sync 只在"发出"上有序，不在"落盘"上有序；
//          掉电时 SB 先到、数据页没到 ⇒ 挂载到一个指向垃圾页的"完整提交"。修法：write_sb = wait_io + sync + 写 SB + sync。
//   坑 82：shrink 的 SB 与上一提交点同版本（vers_ 没前移）。
//   坑 83：SB 槽位取 vers_ & 1，而 commit(false) 只前移 vers_ 不写 SB ⇒ 奇数个逻辑提交之后新 SB 盖掉上一个持久提交的槽，
//          撕裂 ⇒ 回到更老的提交点（已持久的 commit 丢了）。修法：槽位按"上一次 SB 写在哪"交替（sb_slot_）。
//   坑 84：shrink 先截断设备再写 SB；截断到了、SB 没到 ⇒ 老 SB 的 tail 大于设备长度，alloc_pages 从空闲表拿到
//          设备之外的页（IO）。修法：mount 时把设备长回 tail_。
// 测试自身的假阳性（已修）：设备死后的 truncate 也改了 stable_，把上一提交点引用的页截掉了。
// 用法：build/g6 [seed] [ops]；make test 跑 4 个种子（seed%3==0 的种子平均每 20 次写掉一次电，其余每 150 次）。
// 调试：G6_LOG=1 逐步打印并在掉电时倒出追踪环；G6_WATCH=<pid> 打印某一页的 write/settle/truncate 轨迹。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <random>
#include <mutex>
#include <algorithm>
using namespace fcs;

static int fails = 0;
static Store* g_tr = nullptr;
static int g_seed = 0, g_op = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("  FAIL seed=%d op=%d: ", g_seed, g_op); std::printf(__VA_ARGS__); std::printf("\n"); \
    if (fails == 1 && g_tr) g_tr->trace_dump(stderr, 400); } } while (0)

struct VolDev : RamDev {
    std::vector<u8> stable_, dirty_;
    std::mt19937_64 rng{1};
    std::mutex mu_;                       // 异步模式下后台线程也会写
    u64 writes = 0, cut_at = ~0ull;
    bool dead = false, whole = false;
    u64 n_cut = 0, n_kept = 0, n_drop = 0, n_torn = 0;
    explicit VolDev(PageId n) : RamDev(n), stable_((size_t)n * PAGE, 0), dirty_(n, 0), stable_np_(n) {
        const char* w = std::getenv("G6_WATCH"); watch_ = w ? (PageId)atoi(w) : (PageId)~0u;
    }
    PageId watch_;
    void ev(const char* what, PageId pid, u32 extra = 0) {
        if (pid != watch_) return;
        std::fprintf(stderr, "      [dev] %s pid=%u extra=%u writes=%llu dead=%d\n", what, pid, extra, (unsigned long long)writes, (int)dead);
    }
    bool whole_pages() const override { return whole; }
    void settle(PageId p) { ev("settle", p, rd32(raw(p))); std::memcpy(&stable_[(size_t)p * PAGE], raw(p), PAGE); }
    // 掉电瞬间：自上次 sync 以来的脏页各自独立地到达/丢失（页缓存回写无序）
    void cut_now() {
        dead = true; ++n_cut;
        for (PageId p = 0; p < (PageId)dirty_.size(); ++p) if (dirty_[p]) {
            if (rng() & 1) { settle(p); ++n_kept; } else ++n_drop;
            dirty_[p] = 0;
        }
    }
    // 正在写的这一笔：全到 / 全丢 / 撕裂
    void torn(PageId pid, u32 off, const void* src, u32 len) {
        u64 r = rng() % 3;
        if (r == 0) std::memcpy(&stable_[(size_t)pid * PAGE + off], src, len);
        else if (r == 1 && len >= 8) { std::memcpy(&stable_[(size_t)pid * PAGE + off], src, (len / 8) * 4); ++n_torn; }
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        std::lock_guard<std::mutex> g(mu_);
        RC rc = RamDev::do_write(pid, off, src, len);
        ev("write", pid, len);
        if (rc != RC::OK) std::fprintf(stderr, "      [dev] write FAILED pid=%u off=%u len=%u np=%zu dead=%d\n", pid, off, len, dirty_.size(), (int)dead);
        if (rc != RC::OK || dead) return rc;
        if (++writes == cut_at) { cut_now(); torn(pid, off, src, len); return RC::OK; }
        dirty_[pid] = 1;
        return RC::OK;
    }
    RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) override {
        std::lock_guard<std::mutex> g(mu_);
        RC rc = RamDev::do_write_multi(pid, n, src, calls);
        for (u32 i = 0; i < n; ++i) ev("write_multi", pid + i, n);
        if (rc != RC::OK) std::fprintf(stderr, "      [dev] write_multi FAILED pid=%u n=%u np=%zu dead=%d\n", pid, n, dirty_.size(), (int)dead);
        if (rc != RC::OK || dead) return rc;
        if (++writes == cut_at) {
            cut_now();
            for (u32 i = 0; i < n; ++i) torn(pid + i, 0, (const u8*)src + (size_t)i * PAGE, PAGE);
            return RC::OK;
        }
        for (u32 i = 0; i < n; ++i) dirty_[pid + i] = 1;
        return RC::OK;
    }
    void do_sync() override {
        std::lock_guard<std::mutex> g(mu_);
        if (dead) return;
        for (PageId p = 0; p < (PageId)dirty_.size(); ++p) if (dirty_[p]) { settle(p); dirty_[p] = 0; }
    }
    // 简化：截断/增长本身视为立即持久（文件系统里 ftruncate 走元数据日志，顺序上和数据回写一样任意，
    // 但引擎对"截断到了、SB 没到"和"SB 到了、截断没到"两种结局都已处理：mount 把 tail 夹到容量内）。
    // 设备死了以后当然什么都不持久：第一版把死后的截断也写进 stable_，把上一提交点引用的页截掉了（假阳性）。
    bool do_truncate(PageId np) override {
        std::lock_guard<std::mutex> g(mu_);
        if (!RamDev::do_truncate(np)) return false;
        if (watch_ != (PageId)~0u) std::fprintf(stderr, "      [dev] truncate -> %u pages (was %zu) dead=%d\n", np, dirty_.size(), (int)dead);
        dirty_.resize(np, 0);
        if (!dead) { stable_.resize((size_t)np * PAGE, 0); stable_np_ = np; }
        return true;
    }
    PageId stable_np_;
    bool is_dead() { std::lock_guard<std::mutex> g(mu_); return dead; }
    void reboot(u64 span) {
        std::lock_guard<std::mutex> g(mu_);
        RamDev::do_truncate(stable_np_);                 // 页缓存 <- 介质（长度也按介质的来）
        std::memcpy(raw(0), stable_.data(), stable_.size());
        dirty_.assign(stable_np_, 0);
        dead = false; writes = 0; cut_at = 1 + rng() % span;
        if (watch_ != (PageId)~0u) std::fprintf(stderr, "      [dev] reboot: pages=%zu stable[%u] magic=%08x\n", dirty_.size(), watch_,
                                               watch_ < dirty_.size() ? rd32(&stable_[(size_t)watch_ * PAGE]) : 0u);
    }
};

using Truth = std::map<u64, std::vector<u8>>;

static bool same_val(const std::vector<u8>& a, const u8* b, u16 len) {
    return a.size() == len && (len == 0 || std::memcmp(a.data(), b, len) == 0);
}
static u32 compare_all(Store& s, const Truth& t, u64 kmax, const char* what, bool quiet = false) {
    std::vector<u8> out(70000);
    u32 bad = 0; u16 l = 0;
    for (u64 k = 1; k <= kmax; ++k) {
        auto it = t.find(k);
        RC rc = s.get(k, out.data(), (u16)65535, &l);
        bool ok;
        if (it == t.end()) ok = (rc == RC::NOTFOUND);
        else ok = (rc == RC::OK && same_val(it->second, out.data(), l));
        if (!ok) {
            if (bad < 3 && !quiet) std::printf("    %s: key %llu %s (rc=%s len=%u want=%s/%zu)\n", what, (unsigned long long)k,
                                     it == t.end() ? "should be absent" : "wrong/missing", rc_str(rc), l,
                                     it == t.end() ? "-" : "present", it == t.end() ? (size_t)0 : it->second.size());
            ++bad;
        }
    }
    return bad;
}

static int run(int seed, int ops) {
    g_seed = seed;
    std::mt19937_64 rng((u64)seed * 0x9E3779B97F4A7C15ull + 777);
    auto R = [&](u64 n) { return (u64)(rng() % n); };
    const u64 KMAX = 2000;
    const bool async_on = (seed % 2) == 1;
    const u32 io_threads = (seed % 4) >= 2 ? 2 : 1;
    const u32 arena_pages = (seed % 3 == 0) ? 32 : 128;
    const u64 span = (seed % 3 == 0) ? 40 : 300;          // 平均每 span/2 次写掉一次电
    static std::vector<u8> arena;
    arena.assign((size_t)arena_pages * PAGE, 0);
    VolDev dev(16384);
    dev.rng.seed((u64)seed * 1315423911ull + 1);
    dev.whole = (seed % 2) == 0;
    dev.cut_at = 1 + dev.rng() % span;

    Store* s = new Store();
    auto setup = [&](Store* st) {
        if (st->open(&dev, arena.data(), arena_pages) != RC::OK) return false;
        st->set_async_flush_min(1);
        st->set_io_threads(io_threads);
        if (async_on) st->set_async_writes(true);
        g_tr = st;
        return true;
    };
    if (!setup(s)) { std::printf("open fail\n"); return 1; }

    Truth truth, durable, durable_prev;
    std::vector<Truth> snaps;
    bool dchanged = false;                                // 本步里 durable 是否前移过
    u32 n_put = 0, n_del = 0, n_rng = 0, n_commit = 0, n_lcommit = 0, n_snap = 0, n_rb = 0, n_forget = 0,
        n_reopen = 0, n_maint = 0, n_busy = 0, n_verify = 0, n_scrub = 0, n_get = 0, n_sel = 0, n_nospace = 0,
        n_cut = 0, n_cut_cp = 0, n_before = 0, n_after = 0, n_cut_open = 0;
    std::vector<u8> v, out(70000);

    auto mkval = [&](u64 k, int step) {
        u16 len;
        u64 r = R(100);
        if (r < 2) len = (u16)(4100 + R(5000));
        else if (r < 10) len = (u16)(1 + R(8));
        else len = (u16)(8 + R(112));
        v.resize(len);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (u8)(k * 31 + i * 7 + step * 131 + seed);
        return len;
    };
    auto durable_point = [&]() { if (!dchanged) { durable_prev = durable; dchanged = true; } durable = truth; };

    static const bool g_log = std::getenv("G6_LOG") != nullptr;
#define LOG(...) do { if (g_log) { std::fprintf(stderr, "op %d: ", op); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

    // 掉电后的重开与判定。cp：本步是否是提交点类操作。
    auto crash_reopen = [&](int op, bool cp, const char* opname) -> bool {
        const Truth before = dchanged ? durable_prev : durable;   // 本步之前的 durable
        const Truth after = truth;                                 // 本步自己的提交点（若有）
        if (g_log) { std::fprintf(stderr, "op %d: POWER CUT during %s (cp=%d)\n", op, opname, (int)cp); s->trace_dump(stderr, 120); }
        ++n_cut; if (cp) ++n_cut_cp;
        delete s; s = nullptr; g_tr = nullptr;
        for (int again = 0; ; ++again) {
            dev.reboot(span);
            s = new Store();
            if (!setup(s)) { CHECK(false, "open after power cut in %s failed", opname); return false; }
            if (!dev.is_dead()) break;                             // 掉电落在 open 自己的写里：再掉一次
            ++n_cut_open;
            if (again > 8) { CHECK(false, "open keeps dying"); return false; }
            delete s; s = nullptr; g_tr = nullptr;
        }
        snaps.clear();
        u32 b0 = compare_all(*s, before, KMAX, "cut vs before", true);
        u32 b1 = cp ? compare_all(*s, after, KMAX, "cut vs after", true) : ~0u;
        if (b0 == 0) { truth = durable = before; ++n_before; }
        else if (cp && b1 == 0) { truth = durable = after; ++n_after; }
        else {
            CHECK(false, "power cut in %s: recovered state matches neither the previous commit point (%u keys differ) nor this op's (%u)",
                  opname, b0, b1);
            compare_all(*s, before, KMAX, "vs before");
            return false;
        }
        Report rep;
        RC rc = s->verify(&rep);
        CHECK(rc == RC::OK && rep.errors == 0, "verify after power cut in %s: %s (key %llu)", opname, rep.first_err, (unsigned long long)rep.first_key);
        u32 bad = 0, first = 0;
        rc = s->scrub(&bad, &first);
        CHECK(rc == RC::OK && bad == 0, "scrub after power cut in %s: rc=%s bad=%u first=%u", opname, rc_str(rc), bad, first);
        return fails == 0;
    };

    for (int op = 0; op < ops; ++op) {
        g_op = op;
        s->trace_mark((u32)op);
        dchanged = false;
        bool cp = false;
        const char* opname = "?";
        u64 r = R(1000);
        if (r < 480) {
            u64 k = 1 + R(KMAX);
            u16 len = mkval(k, op);
            opname = "put";
            LOG("put %llu len=%u", (unsigned long long)k, len);
            RC rc = s->put(k, v.data(), len);
            if (rc == RC::NOSPACE) { ++n_nospace; cp = true; opname = "put+consolidate"; if (s->consolidate() == RC::OK) durable_point(); }
            else {
                CHECK(rc == RC::OK, "put rc=%s", rc_str(rc));
                if (rc == RC::OK) truth[k] = v;
                ++n_put;
            }
        } else if (r < 560) {
            u64 k = 1 + R(KMAX);
            opname = "del";
            LOG("del %llu", (unsigned long long)k);
            CHECK(s->del(k) == RC::OK, "del");
            truth.erase(k); ++n_del;
        } else if (r < 580) {
            u64 lo = 1 + R(KMAX), hi = std::min<u64>(KMAX + 1, lo + 1 + R(40));
            opname = "del_range";
            LOG("del_range [%llu,%llu)", (unsigned long long)lo, (unsigned long long)hi);
            CHECK(s->del_range(lo, hi) == RC::OK, "del_range");
            truth.erase(truth.lower_bound(lo), truth.lower_bound(hi)); ++n_rng;
        } else if (r < 700) {
            u64 k = 1 + R(KMAX);
            u16 l = 0;
            opname = "get";
            RC rc = s->get(k, out.data(), (u16)65535, &l);
            auto it = truth.find(k);
            if (it == truth.end()) CHECK(rc == RC::NOTFOUND, "get %llu should be NOTFOUND, rc=%s", (unsigned long long)k, rc_str(rc));
            else CHECK(rc == RC::OK && same_val(it->second, out.data(), l), "get %llu wrong (rc=%s len=%u want %zu)", (unsigned long long)k, rc_str(rc), l, it->second.size());
            ++n_get;
        } else if (r < 730) {
            u64 lo = 1 + R(KMAX), hi = std::min<u64>(KMAX + 1, lo + 1 + R(60));
            opname = "select";
            auto it = truth.lower_bound(lo);
            u32 bad = 0, rows = 0; u64 badk = 0;
            s->select(lo, hi, [&](u64 k, const u8* val, u16 len) {
                if (it == truth.end() || it->first != k || !same_val(it->second, val, len)) { if (!bad) badk = k; ++bad; }
                else ++it;
                ++rows; return true;
            });
            if (it != truth.lower_bound(hi)) { if (!bad) badk = it->first; ++bad; }
            CHECK(bad == 0, "select [%llu,%llu) mismatch rows=%u first bad key %llu", (unsigned long long)lo, (unsigned long long)hi, rows, (unsigned long long)badk);
            ++n_sel;
        } else if (r < 790) {
            opname = "commit"; cp = true;
            LOG("commit");
            RC rc = s->commit();
            CHECK(rc == RC::OK, "commit rc=%s", rc_str(rc));
            if (rc == RC::OK) durable_point();
            ++n_commit;
        } else if (r < 840) {
            opname = "commit(false)";
            LOG("commit(false)");
            RC rc = s->commit(false);
            CHECK(rc == RC::OK, "commit(false) rc=%s", rc_str(rc));
            ++n_lcommit;
        } else if (r < 865) {
            if (snaps.size() < MAX_SNAP) {
                opname = "snapshot"; cp = true;
                LOG("snapshot");
                RC rc = s->snapshot();
                CHECK(rc == RC::OK, "snapshot rc=%s", rc_str(rc));
                if (rc == RC::OK) { durable_point(); snaps.push_back(truth); }
                ++n_snap;
            }
        } else if (r < 880) {
            if (!snaps.empty()) {
                u32 id = (u32)R(snaps.size());
                opname = "rollback"; cp = true;
                LOG("rollback %u", id);
                RC rc = s->rollback(id);
                CHECK(rc == RC::OK, "rollback(%u) rc=%s", id, rc_str(rc));
                if (rc == RC::OK) {
                    truth = snaps[id];
                    snaps.resize(id);
                    durable_point();
                    if (!dev.is_dead()) {
                        u32 bad = compare_all(*s, truth, KMAX, "after rollback");
                        CHECK(bad == 0, "rollback(%u): %u keys differ from snapshot truth", id, bad);
                    }
                }
                ++n_rb;
            }
        } else if (r < 895) {
            opname = "forget";
            CHECK(s->forget_snapshots() == RC::OK, "forget");
            snaps.clear(); ++n_forget;
        } else if (r < 955) {
            u64 m = R(5);
            RC rc = RC::OK;
            const char* nm = "";
            LOG("maint %llu", (unsigned long long)m);
            if (m == 0) { nm = "consolidate"; rc = s->consolidate(); }
            else if (m == 1) { nm = "relocate"; rc = s->relocate(); }
            else if (m == 2) { nm = "checkpoint"; rc = s->checkpoint(); }
            else if (m == 3) { nm = "compact_file"; rc = s->compact_file(); }
            else { nm = "shrink"; rc = s->shrink(); }
            opname = nm;
            if (rc == RC::BUSY) ++n_busy;
            else {
                CHECK(rc == RC::OK, "%s rc=%s", nm, rc_str(rc));
                if (m != 4) { durable_point(); cp = true; }   // shrink 只写 SB（已提交状态 + 新高水位），不是提交点
                ++n_maint;
                if (!dev.is_dead()) {
                    u32 bad = 0; u16 l = 0;
                    for (int i = 0; i < 64; ++i) {
                        u64 k = 1 + R(KMAX);
                        RC g = s->get(k, out.data(), (u16)65535, &l);
                        auto it = truth.find(k);
                        bool ok = it == truth.end() ? g == RC::NOTFOUND : (g == RC::OK && same_val(it->second, out.data(), l));
                        if (!ok) ++bad;
                    }
                    CHECK(bad == 0, "after %s: %u/64 sampled keys wrong", nm, bad);
                }
            }
        } else if (r < 965) {
            opname = "scrub";
            u32 bad = 0, first = 0;
            RC rc = s->scrub(&bad, &first);
            CHECK(rc == RC::OK && bad == 0, "scrub rc=%s bad=%u first=%u", rc_str(rc), bad, first);
            ++n_scrub;
        } else if (r < 975) {                             // 干净重开（不掉电）：必须恰好等于 durable
            opname = "reopen";
            LOG("reopen");
            delete s; g_tr = nullptr;
            s = new Store();
            if (!setup(s)) { std::printf("reopen fail\n"); return 1; }
            snaps.clear();
            if (!dev.is_dead()) {
                u32 bad_d = compare_all(*s, durable, KMAX, "reopen vs durable");
                CHECK(bad_d == 0, "reopen: state differs from last commit point (%u keys)", bad_d);
                truth = durable;
            }
            ++n_reopen;
        } else {
            opname = "verify";
            Report rep;
            RC rc = s->verify(&rep);
            CHECK(rc == RC::OK && rep.errors == 0, "verify: %s (key %llu)", rep.first_err, (unsigned long long)rep.first_key);
            ++n_verify;
        }
        if (fails) break;
        if (dev.is_dead()) {                              // 本步里掉电了：重开并判定
            if (!crash_reopen(op, cp, opname)) break;
        }
        if ((op + 1) % 2000 == 0) {
            u32 bad = compare_all(*s, truth, KMAX, "periodic");
            CHECK(bad == 0, "periodic full compare: %u keys differ", bad);
        }
        if (fails) break;
    }
    if (!fails) {
        dev.cut_at = ~0ull;                                // 收尾不再掉电
        CHECK(s->commit() == RC::OK, "final commit");
        Report rep; s->verify(&rep);
        CHECK(rep.errors == 0, "final verify: %s", rep.first_err);
        u32 bad = compare_all(*s, truth, KMAX, "final");
        CHECK(bad == 0, "final compare: %u keys differ", bad);
    }
    std::printf("seed %d: ops=%d async=%d io_threads=%u whole=%d span=%llu | put %u del %u rng %u get %u sel %u | commit %u lcommit %u "
                "snap %u rb %u forget %u reopen %u | maint %u busy %u scrub %u verify %u nospace %u | cuts %u (in commit-point ops %u, in open %u) "
                "-> before %u after %u | pages kept %llu dropped %llu torn %llu | used=%u runs=%u %s\n",
                seed, ops, (int)async_on, io_threads, (int)dev.whole, (unsigned long long)span, n_put, n_del, n_rng, n_get, n_sel, n_commit, n_lcommit,
                n_snap, n_rb, n_forget, n_reopen, n_maint, n_busy, n_scrub, n_verify, n_nospace, n_cut, n_cut_cp, n_cut_open, n_before, n_after,
                (unsigned long long)dev.n_kept, (unsigned long long)dev.n_drop, (unsigned long long)dev.n_torn,
                s->used_pages(), s->run_count(), fails ? "FAIL" : "ok");
    g_tr = nullptr;
    delete s;
    return fails ? 1 : 0;
}

#ifndef G6_NO_MAIN
int main(int argc, char** argv) {
    int ops = argc > 2 ? atoi(argv[2]) : 12000;
    if (argc > 1) { int rc = run(atoi(argv[1]), ops); std::printf("%s\n", fails ? "FAIL" : "PASS"); return rc; }
    for (int seed = 1; seed <= 4 && !fails; ++seed) run(seed, ops);
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
#endif  // G6_NO_MAIN（tests/h3.cpp 复用 VolDev）
