// g5 —— 交错模糊测试（A3，"mini model checker"）
//
// 以前的 bug 全在交叉处：坑 67/68/69 都是 commit(false) × {snapshot, rollback, reopen} 的组合，s2 长跑走过那些
// 路却因为"回退后 resync 真值"没看见。这一段把 op 流和所有维护/恢复入口**随机交错**，每一步都对着模型查：
//   写：put（小值 / 2% 溢出大值）、del、del_range（小段）
//   读：get / select 与真值逐键比对
//   提交：commit()（持久）、commit(false)（逻辑；异步模式下 flush_min=1 ⇒ 每次都刷出 run）
//   恢复：snapshot / rollback(id) / forget_snapshots / reopen（析构再 open 同一设备）
//   维护：consolidate / relocate / checkpoint / compact_file / shrink / scrub
//   设备：RamDev 子类，随机慢写（20~200 µs），whole_pages 随种子切换（整页合并写 / 按用量写两条路径都走）
// 模型（都是逐键精确比对，不是"不崩就行"）：
//   truth    = 当前逻辑状态：put/del/del_range 一经调用就对 get **和 select/count** 可见（坑 76/77 之后的规则）
//   durable  = 上一个提交点的状态。提交点 = commit() / snapshot() / rollback() / 维护操作（consolidate、relocate、
//              checkpoint、compact_file 先 commit() 再动页 —— 坑 78 之后的规则；shrink 只写 SB，不是提交点）
//   快照 k   = snapshot() 时刻的 truth
//   reopen 之后恢复出来的状态必须**恰好**等于 durable（commit(false) 之后未经提交点的写一律不保证）
//   rollback(k) 之后 truth == 快照 k；每 N 步 verify()（含页所有权）。
// 第一轮就抓到 7 个 bug（坑 74–80，见 DESIGN3 §12）：shrink 截断未提交大值的链、del_range 同批后写丢失、
// del_range/select 的可见性、维护操作把未提交墓碑"半持久化"、范围墓碑下点墓碑的账、重放时同 (seq,page0) 的段删除串代。
// 用法：build/g5 [seed] [ops]；make test 跑 4 个种子。失败时打印种子、步号并倒出追踪环。
// 调试：G5_LOG=1 逐步打印操作并在 reopen 前倒追踪环；G5_VERIFY_EVERY=1 每步 verify（找账分家的那一步）；
//       FCS_VERIFY_DBG=1 让引擎打印 flush 的逐条 osz 与 verify 的逐条存活记录。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <random>
#include <thread>
#include <atomic>
#include <chrono>
using namespace fcs;

static int fails = 0;
static Store* g_tr = nullptr;
static int g_seed = 0, g_op = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("  FAIL seed=%d op=%d: ", g_seed, g_op); std::printf(__VA_ARGS__); std::printf("\n"); \
    if (fails == 1 && g_tr) g_tr->trace_dump(stderr, 400); } } while (0)

struct FuzzDev : RamDev {
    std::atomic<u64> x{0x9E3779B97F4A7C15ull};
    int slow_pct = 20;
    bool whole = false;
    explicit FuzzDev(PageId n) : RamDev(n) {}
    bool whole_pages() const override { return whole; }
    void maybe_sleep() {
        u64 v = x.fetch_add(0x9E3779B97F4A7C15ull) ^ 0xD1B54A32D192ED03ull;
        v ^= v >> 29; v *= 0xBF58476D1CE4E5B9ull; v ^= v >> 32;
        if ((int)(v % 100) < slow_pct) std::this_thread::sleep_for(std::chrono::microseconds(20 + (int)((v >> 8) % 180)));
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        maybe_sleep();
        return RamDev::do_write(pid, off, src, len);
    }
    RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) override {
        maybe_sleep();
        return RamDev::do_write_multi(pid, n, src, calls);
    }
};

using Truth = std::map<u64, std::vector<u8>>;

static bool same_val(const std::vector<u8>& a, const u8* b, u16 len) {
    return a.size() == len && (len == 0 || std::memcmp(a.data(), b, len) == 0);
}
// 逐键比对：返回不一致的键数（最多打印 3 个）
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
    std::mt19937_64 rng((u64)seed * 0x9E3779B97F4A7C15ull + 12345);
    auto R = [&](u64 n) { return (u64)(rng() % n); };
    const u64 KMAX = 2000;
    const bool async_on = (seed % 2) == 1;
    const u32 io_threads = (seed % 4) >= 2 ? 2 : 1;
    const u32 arena_pages = (seed % 3 == 0) ? 32 : 128;
    static std::vector<u8> arena;
    arena.assign((size_t)arena_pages * PAGE, 0);
    FuzzDev dev(16384);
    dev.whole = (seed % 2) == 0;
    dev.slow_pct = (seed % 5 == 0) ? 50 : 20;

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

    Truth truth, durable;
    std::vector<Truth> snaps;                 // 快照 k 的真值（与引擎的 nsnap_ 对齐）
    u32 n_put = 0, n_del = 0, n_rng = 0, n_commit = 0, n_lcommit = 0, n_snap = 0, n_rb = 0, n_forget = 0,
        n_reopen = 0, n_maint = 0, n_busy = 0, n_verify = 0, n_scrub = 0, n_get = 0, n_sel = 0, n_nospace = 0;
    std::vector<u8> v, out(70000);

    auto mkval = [&](u64 k, int step) {
        u16 len;
        u64 r = R(100);
        if (r < 2) len = (u16)(4100 + R(5000));               // 溢出大值
        else if (r < 10) len = (u16)(1 + R(8));
        else len = (u16)(8 + R(112));
        v.resize(len);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (u8)(k * 31 + i * 7 + step * 131 + seed);
        return len;
    };
    auto durable_point = [&]() { durable = truth; };
    // 维护操作（consolidate / relocate / checkpoint / compact_file）= 提交点（坑 78 之后的规则）
    auto after_maint = [&]() { durable_point(); };

    static const bool g_log = std::getenv("G5_LOG") != nullptr;
    static const bool g_verify_every = std::getenv("G5_VERIFY_EVERY") != nullptr;
#define LOG(...) do { if (g_log) { std::fprintf(stderr, "op %d: ", op); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)
    for (int op = 0; op < ops; ++op) {
        g_op = op;
        s->trace_mark((u32)op);
        u64 r = R(1000);
        if (r < 480) {                                    // put
            u64 k = 1 + R(KMAX);
            u16 len = mkval(k, op);
            LOG("put %llu len=%u", (unsigned long long)k, len);
            RC rc = s->put(k, v.data(), len);
            if (rc == RC::NOSPACE) { ++n_nospace; s->consolidate(); continue; }
            CHECK(rc == RC::OK, "put rc=%s", rc_str(rc));
            if (rc == RC::OK) truth[k] = v;
            ++n_put;
        } else if (r < 560) {                             // del
            u64 k = 1 + R(KMAX);
            LOG("del %llu", (unsigned long long)k);
            CHECK(s->del(k) == RC::OK, "del");
            truth.erase(k); ++n_del;
        } else if (r < 580) {                             // del_range 小段
            u64 lo = 1 + R(KMAX), hi = std::min<u64>(KMAX + 1, lo + 1 + R(40));
            LOG("del_range [%llu,%llu)", (unsigned long long)lo, (unsigned long long)hi);
            CHECK(s->del_range(lo, hi) == RC::OK, "del_range");
            truth.erase(truth.lower_bound(lo), truth.lower_bound(hi)); ++n_rng;
        } else if (r < 700) {                             // get 对拍
            u64 k = 1 + R(KMAX);
            u16 l = 0;
            RC rc = s->get(k, out.data(), (u16)65535, &l);
            auto it = truth.find(k);
            if (it == truth.end()) CHECK(rc == RC::NOTFOUND, "get %llu should be NOTFOUND, rc=%s", (unsigned long long)k, rc_str(rc));
            else CHECK(rc == RC::OK && same_val(it->second, out.data(), l), "get %llu wrong (rc=%s len=%u want %zu)", (unsigned long long)k, rc_str(rc), l, it->second.size());
            ++n_get;
        } else if (r < 730) {                             // select 对拍
            u64 lo = 1 + R(KMAX), hi = std::min<u64>(KMAX + 1, lo + 1 + R(60));
            auto it = truth.lower_bound(lo);
            u32 bad = 0, rows = 0; u64 badk = 0; const char* why = "";
            s->select(lo, hi, [&](u64 k, const u8* val, u16 len) {
                if (it == truth.end() || it->first != k || !same_val(it->second, val, len)) {
                    if (!bad) { badk = k; why = it == truth.end() ? "extra(end)" : it->first != k ? "key order/extra" : "value"; }
                    ++bad;
                } else ++it;
                ++rows; return true;
            });
            if (it != truth.lower_bound(hi)) { if (!bad) { badk = it->first; why = "missing"; } ++bad; }
            CHECK(bad == 0, "select [%llu,%llu) mismatch rows=%u first bad key %llu (%s)", (unsigned long long)lo, (unsigned long long)hi, rows,
                  (unsigned long long)badk, why);
            ++n_sel;
        } else if (r < 780) {                             // commit()
            LOG("commit");
            RC rc = s->commit();
            CHECK(rc == RC::OK, "commit rc=%s", rc_str(rc));
            if (rc == RC::OK) durable_point();
            ++n_commit;
        } else if (r < 840) {                             // commit(false)
            LOG("commit(false)");
            RC rc = s->commit(false);
            CHECK(rc == RC::OK, "commit(false) rc=%s", rc_str(rc));
            ++n_lcommit;
        } else if (r < 865) {                             // snapshot
            if (snaps.size() < MAX_SNAP) {
                LOG("snapshot");
                RC rc = s->snapshot();
                CHECK(rc == RC::OK, "snapshot rc=%s", rc_str(rc));
                if (rc == RC::OK) { durable_point(); snaps.push_back(truth); }
                ++n_snap;
            }
        } else if (r < 880) {                             // rollback
            if (!snaps.empty()) {
                u32 id = (u32)R(snaps.size());
                LOG("rollback %u", id);
                RC rc = s->rollback(id);
                CHECK(rc == RC::OK, "rollback(%u) rc=%s", id, rc_str(rc));
                if (rc == RC::OK) {
                    truth = snaps[id];
                    snaps.resize(id);                     // 引擎：nsnap_ = id
                    durable_point();
                    u32 bad = compare_all(*s, truth, KMAX, "after rollback");
                    CHECK(bad == 0, "rollback(%u): %u keys differ from snapshot truth", id, bad);
                }
                ++n_rb;
            }
        } else if (r < 900) {                             // forget_snapshots
            CHECK(s->forget_snapshots() == RC::OK, "forget");
            snaps.clear(); ++n_forget;
        } else if (r < 955) {                             // 维护
            u64 m = R(7);
            RC rc = RC::OK;
            const char* nm = "";
            LOG("maint %llu", (unsigned long long)m);
            if (m == 0) { nm = "consolidate"; rc = s->consolidate(); }
            else if (m == 1) { nm = "relocate"; rc = s->relocate(); }
            else if (m == 2) { nm = "checkpoint"; rc = s->checkpoint(); }
            else if (m == 3) { nm = "compact_file"; rc = s->compact_file(); }
            else if (m == 5) { nm = "consolidate_step"; rc = s->consolidate_step(); if (rc == RC::MORE) rc = RC::OK; }     // R2：一步 = 一个提交点
            else if (m == 6) { nm = "compact_file_step"; rc = s->compact_file_step(); if (rc == RC::MORE) rc = RC::OK; }
            else { nm = "shrink"; rc = s->shrink(); }
            if (rc == RC::BUSY) ++n_busy;
            else {
                CHECK(rc == RC::OK, "%s rc=%s", nm, rc_str(rc));
                if (m != 4) after_maint();                // shrink 只写 SB，不走 commit_delta
                ++n_maint;
                // 维护之后抽查 64 个键
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
        } else if (r < 965) {                             // scrub
            LOG("scrub");
            u32 bad = 0, first = 0;
            RC rc = s->scrub(&bad, &first);
            CHECK(rc == RC::OK && bad == 0, "scrub rc=%s bad=%u first=%u", rc_str(rc), bad, first);
            ++n_scrub;
        } else if (r < 975) {                             // reopen
            LOG("reopen");
            if (g_log) s->trace_dump(stderr, 120);
            delete s;
            s = new Store();
            if (!setup(s)) { std::printf("reopen fail\n"); return 1; }
            snaps.clear();                                // 快照不跨重开
            // 恢复出来的必须**恰好**等于上一个提交点（commit / snapshot / rollback / 维护操作）
            u32 bad_d = compare_all(*s, durable, KMAX, "reopen vs durable");
            CHECK(bad_d == 0, "reopen: state differs from last commit point (%u keys)", bad_d);
            truth = durable;
            durable_point();
            ++n_reopen;
        } else {                                          // verify（含页所有权）
            Report rep;
            RC rc = s->verify(&rep);
            CHECK(rc == RC::OK && rep.errors == 0, "verify: %s (key %llu)", rep.first_err, (unsigned long long)rep.first_key);
            ++n_verify;
        }
        if (g_verify_every) {                             // 调试：每步都 verify，找账分家的那一步
            Report rep;
            RC rc = s->verify(&rep);
            CHECK(rc == RC::OK && rep.errors == 0, "verify(every): %s (key %llu)", rep.first_err, (unsigned long long)rep.first_key);
        }
        if ((op + 1) % 2000 == 0) {
            u32 bad = compare_all(*s, truth, KMAX, "periodic");
            CHECK(bad == 0, "periodic full compare: %u keys differ", bad);
        }
        if (fails) break;
    }
    if (!fails) {
        CHECK(s->commit() == RC::OK, "final commit");
        Report rep; s->verify(&rep);
        CHECK(rep.errors == 0, "final verify: %s", rep.first_err);
        u32 bad = compare_all(*s, truth, KMAX, "final");
        CHECK(bad == 0, "final compare: %u keys differ", bad);
    }
    std::printf("seed %d: ops=%d async=%d io_threads=%u whole=%d arena=%u | put %u del %u rng %u get %u sel %u | commit %u lcommit %u "
                "snap %u rb %u forget %u reopen %u | maint %u busy %u scrub %u verify %u nospace %u | used=%u live=%llu runs=%u %s\n",
                seed, ops, (int)async_on, io_threads, (int)dev.whole, arena_pages, n_put, n_del, n_rng, n_get, n_sel, n_commit, n_lcommit,
                n_snap, n_rb, n_forget, n_reopen, n_maint, n_busy, n_scrub, n_verify, n_nospace,
                s->used_pages(), (unsigned long long)s->live_page_count(), s->run_count(), fails ? "FAIL" : "ok");
    g_tr = nullptr;
    delete s;
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    int ops = argc > 2 ? atoi(argv[2]) : 12000;
    if (argc > 1) { int rc = run(atoi(argv[1]), ops); std::printf("%s\n", fails ? "FAIL" : "PASS"); return rc; }
    for (int seed = 1; seed <= 4 && !fails; ++seed) run(seed, ops);
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
