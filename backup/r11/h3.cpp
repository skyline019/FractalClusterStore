// h3 —— 多写者 × 异步提交 × 掉电（第 11 轮 H3；g6 是单写者）
//
// 复用 g6 的 VolDev（易失页缓存 + 随机掉电 + 撕裂页）。W=3 个写者线程并发跑：put / del（各自只碰自己的键：k % W == tid）、
// commit(false)（异步模式下 io_threads=2 的后台刷盘）、commit()。设备在随机一次写时掉电 ⇒ 线程各自停手 ⇒ reboot ⇒ 重开。
// 多写者下没有全局"第 i 步"的概念（别人的 commit 也会把我的 put 带到盘上），所以模型按**每个线程自己的下界**来：
//   * 每个键只有一个写者；写者为它记录自己的操作序列 hist[k] = [(seq, put/del) ...]；
//   * 写者自己的 commit() 返回 OK ⇒ 它此前对每个键的最后一次操作已持久 ⇒ lb[k] = 那个下标；
//   * 重开后键 k 的状态必须**恰好等于 hist[k] 中下标 ≥ lb[k] 的某一项**（put ⇒ 值 = [seq][tid][pattern(k,seq)] 全对；
//     del ⇒ NOTFOUND）；从没写过的键必须 NOTFOUND。值的 pattern 按 (k, seq) 推出 —— 半页、串页、复用链都过不了；
//   * 匹配到的那一项成为下一轮的新 lb（单调：已持久的东西不会倒退）；
//   * 重开必须 OK，verify()（含页所有权）+ scrub() 必须干净。
// 用法：build/h3 [seed] [epochs=10]（每种子再跑 4×epochs 轮短 span）；make test 跑 3 个种子（奇数种子 async + io_threads 2，
// 偶数同步 + io_threads 1）；每轮每线程 ≤ 3000 次操作，整段 alarm 300 s 兜底。
#define G6_NO_MAIN
#include "g6.cpp"
#include <atomic>
#include <thread>
#include <unistd.h>
static int (*const h3_keep_g6_run)(int, int) = run;   // 只为压掉"g6::run 未使用"的告警

namespace h3 {

constexpr int W = 3;
constexpr u64 KMAX = 3000;
struct Op { u32 seq; bool put; };
struct Writer {
    std::map<u64, std::vector<Op>> hist;      // 只含自己的键
    std::map<u64, size_t> lb;                 // hist 下标（含）；没有 ⇒ 0（从"从没写过"算起）
    u32 seq = 0;
};

static void make_val(u64 k, u32 seq, int tid, std::vector<u8>& v) {
    const u32 len = 12 + (u32)((k * 2654435761u + seq * 40503u) % 200);
    v.resize(len);
    std::memcpy(v.data(), &seq, 4); u32 t = (u32)tid; std::memcpy(v.data() + 4, &t, 4);
    u64 h = k * 0x9E3779B97F4A7C15ull ^ (u64)seq * 0xC2B2AE3D27D4EB4Full;
    for (u32 i = 8; i < len; ++i) { h ^= h >> 13; h *= 0xff51afd7ed558ccdull; v[i] = (u8)(h >> 56); }
}

static int run(int seed, int epochs, u64 span) {
    std::mt19937_64 rng((u64)seed * 0x9E3779B97F4A7C15ull + 4242 + span);
    const bool async_on = (seed % 2) == 1;
    const u32 io_threads = async_on ? 2 : 1;
    static u8 arena[128 * PAGE];
    VolDev dev(4096);
    dev.whole = (seed % 2) == 0;
    dev.rng.seed((u64)seed * 31 + 5);
    dev.cut_at = 100 + rng() % span;
    Writer wr[W];
    int bad = 0; u32 n_cuts = 0, n_commit = 0, n_lcommit = 0, n_put = 0, n_del = 0;
    Store* s = new Store();
    auto open_store = [&]() -> RC {
        s->set_async_flush_min(1);
        s->set_io_threads(io_threads);
        if (async_on) s->set_async_writes(true);
        return s->open(&dev, arena, 128);
    };
    if (open_store() != RC::OK) { std::printf("  h3 seed %d: 首次 open 失败\n", seed); return 1; }
    for (int ep = 0; ep < epochs && !bad; ++ep) {
        std::atomic<u32> cnt_commit{0}, cnt_lcommit{0}, cnt_put{0}, cnt_del{0};
        std::atomic<bool> stop{false};
        auto body = [&](int tid) {
            Writer& w = wr[tid];
            std::mt19937_64 r((u64)seed * 1000 + ep * 10 + tid);
            std::vector<u8> v;
            for (int i = 0; i < 3000 && !stop.load(); ++i) {
                if (dev.is_dead()) { stop = true; break; }
                const u64 x = r() % 100;
                if (x < 70 || x < 82) {
                    u64 k = 1 + (r() % (KMAX / W)) * W + (u64)tid; if (k > KMAX) k -= W;
                    ++w.seq;
                    RC rc;
                    if (x < 70) { make_val(k, w.seq, tid, v); rc = s->put(k, v.data(), (u16)v.size()); }
                    else rc = s->del(k);
                    if (rc == RC::FULL) { s->commit(); if (x < 70) rc = s->put(k, v.data(), (u16)v.size()); else rc = s->del(k); }
                    if (rc != RC::OK) { --w.seq; continue; }         // 设备死后也可能 OK；其它错误就当没做
                    w.hist[k].push_back(Op{w.seq, x < 70});
                    x < 70 ? ++cnt_put : ++cnt_del;
                } else if (x < 90) {
                    if (s->commit(false) == RC::OK) ++cnt_lcommit;
                } else {
                    if (s->commit() == RC::OK) {
                        ++cnt_commit;
                        if (!dev.is_dead()) for (auto& kv : w.hist) w.lb[kv.first] = kv.second.size() - 1;   // 我此前的操作全持久了
                        // 设备已死时 commit 也返回 OK（进程不知道）—— 但它没落盘，不能提升下界
                    }
                }
            }
        };
        std::vector<std::thread> ts;
        for (int t = 0; t < W; ++t) ts.emplace_back(body, t);
        for (auto& t : ts) t.join();
        n_commit += cnt_commit; n_lcommit += cnt_lcommit; n_put += cnt_put; n_del += cnt_del;
        const bool cut = dev.is_dead();
        if (cut) ++n_cuts;
        // 掉电（或没掉电：本轮结束时也"拔电"一次 —— 相当于最后一次写之后掉电）
        if (!cut) { std::lock_guard<std::mutex> g(dev.mu_); dev.cut_now(); }
        delete s;
        dev.reboot(span);
        s = new Store();
        RC rc = open_store();
        if (rc != RC::OK) { std::printf("  h3 FAIL seed %d ep %d: 重开 rc=%s\n", seed, ep, rc_str(rc)); bad++; break; }
        Report rep; rc = s->verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  h3 FAIL seed %d ep %d: verify rc=%s errors=%u first=%s key=%llu\n", seed, ep, rc_str(rc), rep.errors, rep.first_err, (unsigned long long)rep.first_key); bad++; break; }
        u32 sb = 0, sf = 0; rc = s->scrub(&sb, &sf);
        if (rc != RC::OK || sb) { std::printf("  h3 FAIL seed %d ep %d: scrub rc=%s bad=%u first=%u\n", seed, ep, rc_str(rc), sb, sf); bad++; break; }
        // 逐键比对：状态 ∈ hist[k][lb..]
        std::vector<u8> out(70000), want; u16 len = 0; int shown = 0;
        for (u64 k = 1; k <= KMAX; ++k) {
            Writer& w = wr[(k - 1) % W];
            RC g = s->get(k, out.data(), 65535, &len);
            auto it = w.hist.find(k);
            if (it == w.hist.end()) { if (g != RC::NOTFOUND) { if (shown++ < 3) std::printf("  h3 FAIL seed %d ep %d: 从没写过的键 %llu rc=%s\n", seed, ep, (unsigned long long)k, rc_str(g)); bad++; } continue; }
            std::vector<Op>& h = it->second;
            size_t lo = w.lb.count(k) ? w.lb[k] : 0;
            bool lb_set = w.lb.count(k) > 0;
            long match = -1;
            if (g == RC::NOTFOUND) {
                if (!lb_set) match = -2;                                 // 允许：从没持久过
                for (size_t i = lo; i < h.size() && match < 0; ++i) if (!h[i].put) match = (long)i;
            } else if (g == RC::OK && len >= 8) {
                u32 seq, tid; std::memcpy(&seq, out.data(), 4); std::memcpy(&tid, out.data() + 4, 4);
                for (size_t i = lo; i < h.size(); ++i) if (h[i].put && h[i].seq == seq) {
                    make_val(k, seq, (int)tid, want);
                    if (tid == (k - 1) % W && want.size() == len && std::memcmp(want.data(), out.data(), len) == 0) match = (long)i;
                    break;
                }
            }
            if (match == -1) {
                if (shown++ < 3) std::printf("  h3 FAIL seed %d ep %d: 键 %llu rc=%s len=%u 不是 hist[%zu..%zu) 里的任何一项\n", seed, ep, (unsigned long long)k, rc_str(g), len, lo, h.size());
                bad++; continue;
            }
            // 收敛：匹配项成为新下界，之后的历史作废（没持久的操作就是没发生）
            if (match >= 0) { h.resize((size_t)match + 1); w.lb[k] = (size_t)match; }
            else { h.clear(); w.lb.erase(k); }
        }
        for (auto& w : wr) for (auto it = w.hist.begin(); it != w.hist.end(); ) it = it->second.empty() ? w.hist.erase(it) : std::next(it);
    }
    delete s;
    std::printf("  h3 seed %d span %llu: async=%d io_threads=%u whole=%d | %d 轮 掉电 %u 次 | put %u del %u commit %u lcommit %u | 撕裂 %llu 页  %s\n",
                seed, (unsigned long long)span, (int)async_on, io_threads, (int)dev.whole, epochs, n_cuts, n_put, n_del, n_commit, n_lcommit, (unsigned long long)dev.n_torn, bad ? "FAIL" : "ok");
    return bad;
}
} // namespace h3

int main(int argc, char** argv) {
    alarm(300);
    int epochs = argc > 2 ? atoi(argv[2]) : 10;
    // 每个种子两段：长 span（1500 次写掉一次电：一轮里攒出多条 run、几次层合并）+ 短 span（60：专打提交点窗口，4 倍轮数）。
    // 故障注入自检：把 write_sb() 里 SB 之前的 sync 去掉，短 span 段 3 个种子都在 ≤ 21 轮内报「状态低于持久下界」。
    auto both = [&](int seed) { int b = h3::run(seed, epochs, 1500); if (!b) b = h3::run(seed, epochs * 4, 60); return b; };
    int bad = 0;
    if (argc > 1) bad = both(atoi(argv[1]));
    else for (int seed = 1; seed <= 3 && !bad; ++seed) bad += both(seed);
    std::printf("h3 多写者 × 异步 × 掉电: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
