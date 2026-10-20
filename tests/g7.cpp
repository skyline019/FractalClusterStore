// g7 —— 读写并发护栏（G2；P1"读扩展性"的前置）
//
// 一个写者线程跑 g5 式的 op 流（put / del / del_range / commit / commit(false) / snapshot / forget / 维护 / scrub），
// 两个读者线程不停地 get / select。读者不知道写者此刻写到哪，所以不能逐键比对真值；它们检查的是**不变量**：
//   1) 值是键 k 的某个真实版本：值 = [step(4B)][pattern...]，pattern 由 (k, step, seed) 推出 —— 半页、别人的页、
//      被复用的溢出链都过不了这一关；
//   2) 同一读者看同一键，step 单调不降（单写者、无 rollback ⇒ 可见性单调）；
//   3) select 内键严格递增，每个值同样过 1)；
//   4) 写者结束后逐键全量比对真值，再 verify + scrub。
// 战绩：首跑 seed 1 就抓到坑 85（scrub() 不持锁；同类 get() 锁外 open_vers()、index_scan 无锁）；TSan 版又抓 3 类
//（wait_page 锁外读 inflight_ 要 acquire、RamDev::np_ 要原子、Run::meta_pending 读写竞争）；P1 之后 `3 6000 4` 抓到坑 86
//（std::shared_mutex 读者优先 ⇒ 写者饿死，15 分钟不结束）。
// P1 之后默认开 set_shared_reads(true)：读者走共享锁 + 风险指针页缓存；G7_EXCL=1 测独占老路。
// 用法：build/g7 [seed] [writer_ops] [readers]；make test 跑 2 个种子 + 独占 1 种子 + 4 读者 1 种子；
// make asan 里再以 TSan 编一遍跑三种配置（0 警告才算过：pipefail 会把 TSan 的 rc=66 传出来）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>
using namespace fcs;

static std::atomic<int> fails{0};
static int g_seed = 0;
static std::mutex g_print;
#define CHECK(c, ...) do { if (!(c)) { int f = ++fails; if (f <= 8) { std::lock_guard<std::mutex> _g(g_print); \
    std::printf("  FAIL seed=%d: ", g_seed); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

using Truth = std::map<u64, std::vector<u8>>;

// 值的编码：前 4 字节 = step，后面 pattern(k, step, seed)
static void mkval(std::vector<u8>& v, u64 k, u32 step, u16 len, int seed) {
    v.resize(len);
    std::memcpy(v.data(), &step, 4);
    for (size_t i = 4; i < v.size(); ++i) v[i] = (u8)(k * 31 + i * 7 + step * 131 + seed);
}
// 读者校验：值是不是 k 的某个真实版本；返回 step（不合法返回 -1）
static long long check_val(u64 k, const u8* val, u16 len, int seed) {
    if (len < 4) return -1;
    u32 step; std::memcpy(&step, val, 4);
    for (size_t i = 4; i < len; ++i) if (val[i] != (u8)(k * 31 + i * 7 + step * 131 + seed)) return -1;
    return (long long)step;
}

static int run(int seed, int wops, int nreaders) {
    g_seed = seed;
    const u64 KMAX = 1500;
    const bool async_on = (seed % 2) == 1;
    static std::vector<u8> arena;
    arena.assign((size_t)128 * PAGE, 0);
    RamDev dev(16384);
    Store s;
    if (s.open(&dev, arena.data(), 128) != RC::OK) { std::printf("open fail\n"); return 1; }
    s.set_async_flush_min(1);
    s.set_io_threads(1);
    if (async_on) s.set_async_writes(true);
    const bool shared = std::getenv("G7_EXCL") == nullptr;   // 默认测共享读模式（P1）；G7_EXCL=1 测独占老路
    s.set_shared_reads(shared);

    std::atomic<bool> stop{false};
    std::atomic<u64> n_rget{0}, n_rsel{0}, n_rfound{0};

    // ---------------- 读者
    auto reader = [&](int rid) {
        std::mt19937_64 rng((u64)seed * 7919 + rid * 104729 + 1);
        std::vector<u8> out(70000);
        std::vector<long long> last(KMAX + 1, -1);         // 本读者看到的每键最新 step
        u64 local_get = 0, local_sel = 0, local_found = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            u64 r = rng() % 100;
            if (r < 80) {
                u64 k = 1 + rng() % KMAX;
                u16 l = 0;
                RC rc = s.get(k, out.data(), (u16)65535, &l);
                CHECK(rc == RC::OK || rc == RC::NOTFOUND, "reader %d get %llu rc=%s", rid, (unsigned long long)k, rc_str(rc));
                if (rc == RC::OK) {
                    long long st = check_val(k, out.data(), l, seed);
                    CHECK(st >= 0, "reader %d get %llu: value is not a genuine version (len=%u)", rid, (unsigned long long)k, l);
                    CHECK(st >= last[k], "reader %d get %llu: step went backwards %lld -> %lld", rid, (unsigned long long)k, last[k], st);
                    if (st > last[k]) last[k] = st;
                    ++local_found;
                }
                ++local_get;
            } else {
                u64 lo = 1 + rng() % KMAX, hi = std::min<u64>(KMAX + 1, lo + 1 + rng() % 50);
                u64 prev = 0; u32 rows = 0; bool bad = false;
                RC rc = s.select(lo, hi, [&](u64 k, const u8* val, u16 len) {
                    if (k < lo || k >= hi || k <= prev) bad = true;
                    prev = k;
                    long long st = check_val(k, val, len, seed);
                    if (st < 0) bad = true;
                    else { if (st < last[k]) bad = true; else last[k] = st; }
                    ++rows; return true;
                });
                CHECK(rc == RC::OK && !bad, "reader %d select [%llu,%llu) rc=%s bad=%d rows=%u", rid, (unsigned long long)lo, (unsigned long long)hi, rc_str(rc), (int)bad, rows);
                ++local_sel;
            }
        }
        n_rget += local_get; n_rsel += local_sel; n_rfound += local_found;
    };

    // ---------------- 写者（g5 式，去掉 rollback / reopen）
    Truth truth;
    u32 n_put = 0, n_del = 0, n_rng = 0, n_commit = 0, n_lcommit = 0, n_snap = 0, n_forget = 0, n_maint = 0, n_busy = 0, n_scrub = 0;
    u32 nsnaps = 0;
    auto writer = [&]() {
        std::mt19937_64 rng((u64)seed * 0x9E3779B97F4A7C15ull + 4242);
        auto R = [&](u64 n) { return (u64)(rng() % n); };
        std::vector<u8> v;
        for (int op = 0; op < wops && fails.load() == 0; ++op) {
            u64 r = R(1000);
            if (r < 520) {
                u64 k = 1 + R(KMAX);
                u64 x = R(100);
                u16 len = x < 3 ? (u16)(4100 + R(5000)) : x < 12 ? (u16)(4 + R(5)) : (u16)(8 + R(112));
                mkval(v, k, (u32)op, len, seed);
                RC rc = s.put(k, v.data(), len);
                if (rc == RC::NOSPACE) { s.consolidate(); continue; }
                CHECK(rc == RC::OK, "put rc=%s", rc_str(rc));
                if (rc == RC::OK) truth[k] = v;
                ++n_put;
            } else if (r < 600) {
                u64 k = 1 + R(KMAX);
                CHECK(s.del(k) == RC::OK, "del");
                truth.erase(k); ++n_del;
            } else if (r < 625) {
                u64 lo = 1 + R(KMAX), hi = std::min<u64>(KMAX + 1, lo + 1 + R(40));
                CHECK(s.del_range(lo, hi) == RC::OK, "del_range");
                truth.erase(truth.lower_bound(lo), truth.lower_bound(hi)); ++n_rng;
            } else if (r < 700) {
                RC rc = s.commit();
                CHECK(rc == RC::OK, "commit rc=%s", rc_str(rc)); ++n_commit;
            } else if (r < 780) {
                RC rc = s.commit(false);
                CHECK(rc == RC::OK, "commit(false) rc=%s", rc_str(rc)); ++n_lcommit;
            } else if (r < 810) {
                if (nsnaps < MAX_SNAP) { RC rc = s.snapshot(); CHECK(rc == RC::OK, "snapshot rc=%s", rc_str(rc)); if (rc == RC::OK) ++nsnaps; ++n_snap; }
            } else if (r < 840) {
                CHECK(s.forget_snapshots() == RC::OK, "forget"); nsnaps = 0; ++n_forget;
            } else if (r < 960) {
                u64 m = R(5);
                RC rc = RC::OK;
                const char* nm = "";
                if (m == 0) { nm = "consolidate"; rc = s.consolidate(); }
                else if (m == 1) { nm = "relocate"; rc = s.relocate(); }
                else if (m == 2) { nm = "checkpoint"; rc = s.checkpoint(); }
                else if (m == 3) { nm = "compact_file"; rc = s.compact_file(); }
                else { nm = "shrink"; rc = s.shrink(); }
                if (rc == RC::BUSY) ++n_busy;
                else { CHECK(rc == RC::OK, "%s rc=%s", nm, rc_str(rc)); ++n_maint; }
            } else {
                u32 bad = 0, first = 0;
                RC rc = s.scrub(&bad, &first);
                CHECK(rc == RC::OK && bad == 0, "scrub rc=%s bad=%u first=%u", rc_str(rc), bad, first);
                ++n_scrub;
            }
        }
    };

    std::vector<std::thread> rs;
    for (int i = 0; i < nreaders; ++i) rs.emplace_back(reader, i);
    std::thread w(writer);
    w.join();
    stop = true;
    for (auto& t : rs) t.join();

    // 写者停了：逐键全量比对 + verify + scrub
    if (fails.load() == 0) {
        std::vector<u8> out(70000);
        u32 bad = 0; u16 l = 0;
        for (u64 k = 1; k <= KMAX; ++k) {
            auto it = truth.find(k);
            RC rc = s.get(k, out.data(), (u16)65535, &l);
            bool ok = it == truth.end() ? rc == RC::NOTFOUND
                                        : (rc == RC::OK && it->second.size() == l && std::memcmp(it->second.data(), out.data(), l) == 0);
            if (!ok) ++bad;
        }
        CHECK(bad == 0, "final compare: %u keys differ", bad);
        Report rep; RC rc = s.verify(&rep);
        CHECK(rc == RC::OK && rep.errors == 0, "final verify: %s", rep.first_err);
        u32 sb = 0, sf = 0; rc = s.scrub(&sb, &sf);
        CHECK(rc == RC::OK && sb == 0, "final scrub rc=%s bad=%u", rc_str(rc), sb);
    }
    std::printf("seed %d: writer ops=%d async=%d readers=%d shared=%d | put %u del %u rng %u commit %u lcommit %u snap %u forget %u maint %u busy %u scrub %u | "
                "reader get %llu (found %llu) select %llu | used=%u runs=%u %s\n",
                seed, wops, (int)async_on, nreaders, (int)shared, n_put, n_del, n_rng, n_commit, n_lcommit, n_snap, n_forget, n_maint, n_busy, n_scrub,
                (unsigned long long)n_rget.load(), (unsigned long long)n_rfound.load(), (unsigned long long)n_rsel.load(),
                s.used_pages(), s.run_count(), fails.load() ? "FAIL" : "ok");
    return fails.load() ? 1 : 0;
}

int main(int argc, char** argv) {
    int wops = argc > 2 ? atoi(argv[2]) : 6000;
    int nreaders = argc > 3 ? atoi(argv[3]) : 2;
    if (argc > 1) { int rc = run(atoi(argv[1]), wops, nreaders); std::printf("%s\n", fails.load() ? "FAIL" : "PASS"); return rc; }
    for (int seed = 1; seed <= 2 && !fails.load(); ++seed) run(seed, wops, nreaders);
    std::printf("%s\n", fails.load() ? "FAIL" : "PASS");
    return fails.load() ? 1 : 0;
}
