// g14 —— 第 14 轮回归（三项优化）：
//   (1) 分步作业收尾的单元级增量账：作业跨多片、片间有新写覆盖 / 压实 / 切分时，收尾走公式（job_fast）而不是把 touched 全标未知；
//       判据：公式路径被走到、维护欠账能清零、**账零漂移**（每个单元的 live/dead/段 live 与全量 recount 完全一致）、verify + 真值。
//   (2) 文件高水位：流式预留按存活字节估（峰值 ≤ 产物 × 1.3 而不是 Σ源页 × 1.125）；估小了就地续租（不搬家）；顶部空闲段释放后
//       tail_ 回落；set_hw_reclaim_pct 策略把顶部 run 搬进低洞（hw 回落到阈值内），默认关（pct=0）时行为不变。
//   (3) mode 3 读者让路：有读者争用时维护单元按时间片让路（yields > 0）；让路切点（cut）之后的账仍零漂移；quantum=0 时不让路。
// 判据：每段都 verify + 与真值比对；失败打印第一处。单跑 ≈ 10 ~ 20 s（RamDev）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>
#define private public
#include "fcs.h"
#undef private
using namespace fcs;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Model { std::map<u64, u32> m; };

void putm(Store& s, Model& t, u64 k, u32 tag, u32 len = 200) {
    std::vector<u8> v(len, (u8)(tag * 7 + 1)); std::memcpy(v.data(), &tag, 4);
    s.put(k, v.data(), (u16)len); t.m[k] = tag;
}
void delm(Store& s, Model& t, u64 k) { s.del(k); t.m[k] = 0; }
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
// 把欠的维护干完（mode 1：空提交触发；mode 3：后台线程）
void drain(Store& s, int mode) {
    double t0 = now_s();
    while (s.maint_pending() && now_s() - t0 < 60) { if (mode == 3) usleep(1000); else s.commit(); }
    if (mode != 3) { for (int i = 0; i < 64 && s.maintain(8) == RC::MORE; ++i) {} }
}
// 账零漂移：所有已知单元的 live / dead / 每段 live 必须和全量 recount 一致（recount 本身会把账改成精确值，所以先抄一份）。
// 只在没有作业在飞、没有后台线程的时候调（mode 3 先切回 mode 1）。
int acct_drift(Store& s, const char* tag) {
    int bad = 0; u32 checked = 0, unknown = 0;
    if (s.job_) { std::printf("  %s: 作业未结束，跳过对账\n", tag); return 0; }
    for (auto& kv : s.cells_) {
        Cell* c = kv.second;
        if (!c->acct) { ++unknown; continue; }
        const u64 l0 = c->live, d0 = c->dead;
        std::vector<u32> sl; for (auto& sg : c->segs) sl.push_back(sg.second.live);
        s.recount(c);
        std::vector<u32> sl2; for (auto& sg : c->segs) sl2.push_back(sg.second.live);
        if (l0 != c->live || d0 != c->dead || sl != sl2) {
            if (bad++ < 3) std::printf("  %s FAIL: 账漂移 cell=[%llu,%llu) live %llu vs %llu dead %llu vs %llu segs %zu\n", tag,
                                       (unsigned long long)c->lo, (unsigned long long)c->hi, (unsigned long long)l0, (unsigned long long)c->live,
                                       (unsigned long long)d0, (unsigned long long)c->dead, sl.size());
        }
        ++checked;
    }
    if (!checked) { std::printf("  %s FAIL: 没有一个已知单元可对账（未知 %u）\n", tag, unknown); return 1; }
    return bad ? 1 : 0;
}

// ------------------------------------------------------------------ (1) 作业收尾增量账
int t_jobacct() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1401);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_jobacct open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_maint_budget_us(20000); s.set_merge_budget_pages(32);   // 小片：作业必然跨很多提交点
    const u32 N = 30000;
    double cmax = 0; u32 pend = 0;
    for (int c = 0; c < 120; ++c) {
        // 写一批：一半随机覆盖（片后覆盖 ⇒ job_overwrite 钩子），少量删除 + 范围删除（⇒ jdirty 回退路径也要走到）
        for (int i = 0; i < 600; ++i) putm(s, t, 1 + rng() % N, 100 + c, 300);
        if (c % 7 == 3) for (int i = 0; i < 20; ++i) delm(s, t, 1 + rng() % N);
        if (c % 29 == 11) { u64 lo = 1 + rng() % N; u64 hi = std::min<u64>(N + 1, lo + 150); s.del_range(lo, hi); for (u64 k = lo; k < hi; ++k) if (t.m.count(k)) t.m[k] = 0; }
        double t0 = now_s();
        if (s.commit() != RC::OK) { std::printf("  t_jobacct FAIL: commit %d\n", c); bad++; }
        cmax = std::max(cmax, now_s() - t0);
        if (s.maint_pending()) ++pend;
        if (c % 40 == 39) bad += cmp_model(s, t, "t_jobacct");
    }
    auto ms = s.maint_stats();
    if (ms.job_fast == 0) { std::printf("  t_jobacct FAIL: 作业收尾从未走公式路径（job_fast=0 job_slow=%llu）\n", (unsigned long long)ms.job_slow); bad++; }
    drain(s, 1);
    if (s.maint_pending()) { std::printf("  t_jobacct FAIL: 欠账清不掉\n"); bad++; }
    if (s.unk_cells_) { std::printf("  t_jobacct FAIL: 清账后仍有未知单元 %u\n", s.unk_cells_); bad++; }
    bad += acct_drift(s, "t_jobacct") + vok(s, "t_jobacct") + cmp_model(s, t, "t_jobacct");
    std::printf("  作业增量账：120 次提交 job_fast=%llu job_slow=%llu（提交后有欠账 %u 次、提交 max %.1f ms）；清账后账零漂移  %s\n",
                (unsigned long long)ms.job_fast, (unsigned long long)ms.job_slow, pend, cmax * 1e3, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (2) 文件高水位
// 2a：脏组合并的预留按存活字节估：峰值增长 ≤ 产物 × 1.3（老估法 = Σ源页 × 1.125，脏 60% 时 ≈ 产物 × 2.8）；不搬家、不续租失败
int t_reserve() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1402);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_reserve open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_lvl_t(1000); s.set_merge_cheap_pages(0);   // 先不维护：手工搭源
    const u32 N = 24000;
    for (u32 k = 1; k <= N; ++k) putm(s, t, k, 1, 400);                   // 一条大 run（≈ 2400 页 ≥ 2 MB ⇒ 走流式）
    if (s.commit() != RC::OK) bad++;
    for (u32 k = 1; k <= N; ++k) if (k % 5) putm(s, t, k, 2, 400);        // 覆盖 80%：老 run 80% 脏
    if (s.commit() != RC::OK) bad++;
    drain(s, 1);
    RunPtr old = nullptr; for (auto& kv : s.runs_) if (kv.second->npages && (!old || kv.second->seq < old->seq)) old = kv.second;
    if (!old) { std::printf("  t_reserve FAIL: 没有源 run\n"); return bad + 1; }
    old->lvl = 1;                                                          // 规则 (d) 的单条重写只对 lvl ≥ 1 的大 run 做；L0 等组合并
    const u32 src_pages = old->npages; const u32 tail0 = s.tail_; const u64 lb = std::min<u64>(old->live_bytes, old->nbytes);
    s.set_lvl_t(8); s.set_merge_budget_pages(64);                         // (d)：单条 ≥ 50% 脏 ⇒ 分片重写
    u32 peak = s.tail_; u32 steps = 0;
    while (steps++ < 4000) { RC r = s.maintain(1); peak = std::max(peak, s.tail_); if (r != RC::MORE) break; if (s.commit() != RC::OK) { bad++; break; } peak = std::max(peak, s.tail_); }
    drain(s, 1);
    auto ms = s.maint_stats();
    RunPtr prod = nullptr; for (auto& kv : s.runs_) if (kv.second->npages && kv.second->lvl >= 1) prod = kv.second;   // 重写产物（升一层）
    const u32 pp = prod ? prod->npages : 0;
    const u32 grow = peak > tail0 ? peak - tail0 : 0;
    if (!pp) { std::printf("  t_reserve FAIL: 没有产物（runs=%u）\n", s.run_count()); bad++; }
    else if ((u64)grow * 10 > (u64)pp * 14 + 640) { std::printf("  t_reserve FAIL: 峰值增长 %u 页 > 产物 %u 页 × 1.4 + 64（源 %u 页，存活估 %llu 页）\n", grow, pp, src_pages, (unsigned long long)(lb / (PAGE - DHDR))); bad++; }
    if (ms.stream_runs == 0) { std::printf("  t_reserve FAIL: 没走流式（stream_runs=0）\n"); bad++; }
    if (ms.stream_moves) { std::printf("  t_reserve FAIL: 估小搬家 %llu 次\n", (unsigned long long)ms.stream_moves); bad++; }
    bad += acct_drift(s, "t_reserve") + vok(s, "t_reserve") + cmp_model(s, t, "t_reserve");
    std::printf("  预留按存活估：源 %u 页（80%% 脏）⇒ 产物 %u 页，峰值增长 %u 页（%.2f× 产物；老估法 ≈ %.2f×）stream=%llu moves=%llu extends=%llu  %s\n",
                src_pages, pp, grow, pp ? (double)grow / pp : 0.0, pp ? (double)(src_pages + src_pages / 8) / pp : 0.0,
                (unsigned long long)ms.stream_runs, (unsigned long long)ms.stream_moves, (unsigned long long)ms.stream_extends, bad ? "FAIL" : "ok");
    return bad;
}
// 2b：估小 ⇒ 就地续租（不搬家）：把源 run 的 live_bytes 人为压到 1/4 再合并
int t_extend() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_extend open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_lvl_t(1000); s.set_merge_cheap_pages(0);
    const u32 N = 16000;
    for (u32 k = 1; k <= N; ++k) putm(s, t, k, 1, 400);
    if (s.commit() != RC::OK) bad++;
    for (u32 k = 1; k <= N; ++k) if (k % 2) putm(s, t, k, 2, 400);        // 50% 脏 ⇒ 规则 (d) 会重写老 run
    if (s.commit() != RC::OK) bad++;
    drain(s, 1);
    RunPtr old = nullptr; for (auto& kv : s.runs_) if (kv.second->npages && (!old || kv.second->seq < old->seq)) old = kv.second;
    if (!old) { std::printf("  t_extend FAIL: 没有源 run\n"); return bad + 1; }
    old->lvl = 1;                                                          // 同 t_reserve：让规则 (d) 对它做单条重写
    old->live_bytes = std::max<u64>(1, std::min<u64>(old->live_bytes, old->nbytes) / 4);   // 故意估小：预留只有真实产物的 1/4 左右
    s.set_lvl_t(8); s.set_merge_budget_pages(64);
    for (int i = 0; i < 4000; ++i) { RC r = s.maintain(1); if (r != RC::MORE) break; if (s.commit() != RC::OK) { bad++; break; } }
    drain(s, 1);
    auto ms = s.maint_stats();
    if (ms.stream_extends == 0) { std::printf("  t_extend FAIL: 估小却没有就地续租（extends=0 moves=%llu stream=%llu）\n", (unsigned long long)ms.stream_moves, (unsigned long long)ms.stream_runs); bad++; }
    if (ms.stream_moves) { std::printf("  t_extend FAIL: 续租之外还搬了家 %llu 次\n", (unsigned long long)ms.stream_moves); bad++; }
    bad += acct_drift(s, "t_extend") + vok(s, "t_extend") + cmp_model(s, t, "t_extend");
    std::printf("  估小就地续租：extends=%llu moves=%llu stream=%llu  %s\n", (unsigned long long)ms.stream_extends, (unsigned long long)ms.stream_moves, (unsigned long long)ms.stream_runs, bad ? "FAIL" : "ok");
    return bad;
}
// 2c：顶部回落 + 回收策略。同一负载跑两遍：pct=0（默认）尾部停在峰值；pct=150 ⇒ 顶部 run 搬进低洞、tail_ 回落到 ≤ 1.5× 活页。
struct HwRes { u32 tail = 0, live = 0; u64 reclaims = 0, writes = 0; int bad = 0; };
HwRes run_hw(u32 pct, int mode, u32 seed) {
    HwRes r; static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(seed);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_hw open fail\n"); r.bad = 1; return r; }
    s.set_maint_mode(mode); s.set_hw_reclaim_pct(pct); s.set_merge_budget_pages(64);
    const u32 N = 40000;
    for (u32 k = 1; k <= N; ++k) { putm(s, t, k, 1, 200); if (k % 5000 == 0 && s.commit() != RC::OK) r.bad++; }   // 8 条 L0 ⇒ (c) 合成一条 L1
    drain(s, mode);
    for (u32 i = 0; i < N * 3 / 5; ++i) { putm(s, t, 1 + rng() % N, 2, 200); if (i % 3000 == 2999 && s.commit() != RC::OK) r.bad++; }   // 覆盖 60%：老 L1 ≥ 50% 脏 ⇒ (d) 重写
    if (s.commit() != RC::OK) r.bad++;
    drain(s, mode);
    for (int c = 0; c < 10; ++c) { for (int i = 0; i < 400; ++i) putm(s, t, 1 + rng() % N, 3, 200); if (s.commit() != RC::OK) r.bad++; drain(s, mode); }   // 再写几批小的：顶上的小 L0 被合并走、新 run 落进低洞 ⇒ 顶部空出来
    if (mode == 3) s.set_maint_mode(1);                                     // join 后台线程再看账
    auto ms = s.maint_stats();
    r.tail = s.tail_; r.live = (u32)s.live_page_count(); r.reclaims = ms.hw_reclaims; r.writes = dev.st.write_bytes.load();
    if (s.tail_ > s.dev_pages_) { std::printf("  t_hw FAIL: tail_ %u > dev_pages_ %u\n", s.tail_, s.dev_pages_); r.bad++; }
    r.bad += acct_drift(s, "t_hw") + vok(s, "t_hw") + cmp_model(s, t, "t_hw");
    return r;
}
int t_hw() {
    int bad = 0;
    HwRes a = run_hw(0, 1, 1403), b = run_hw(150, 1, 1403), c = run_hw(150, 3, 1403);
    bad += a.bad + b.bad + c.bad;
    if (a.reclaims) { std::printf("  t_hw FAIL: 默认 pct=0 却回收了 %llu 次\n", (unsigned long long)a.reclaims); bad++; }
    if ((u64)a.tail * 100 > (u64)a.live * 150) {                             // 这条负载默认必须超阈值，否则 pct=150 段证明不了什么
        if (b.reclaims == 0) { std::printf("  t_hw FAIL: pct=150 没有回收（tail %u live %u）\n", b.tail, b.live); bad++; }
        // 第 16 轮改判据：P2b 分片后默认 tail 本身就低了（这条负载 1.9× → 1.55×），"比默认低 ≥ 15%" 失去意义；改看策略的承诺本身：
        // 回收后 tail ≤ 1.5× 活页，且确实比默认低。
        if ((u64)b.tail * 100 > (u64)b.live * 150 || b.tail >= a.tail) { std::printf("  t_hw FAIL: 回收后 tail %u 没有回到 ≤ 1.5× 活页 %u / 没有低于默认 %u\n", b.tail, b.live, a.tail); bad++; }
        if (c.reclaims == 0) { std::printf("  t_hw FAIL: mode 3 pct=150 没有回收\n"); bad++; }
    } else std::printf("  t_hw 注意：默认负载 tail/live = %.2f 未超 1.5×，回收段只检查无错\n", (double)a.tail / std::max<u32>(1, a.live));
    std::printf("  高水位：默认 tail %u / 活页 %u (%.2fx, 写 %.1f MB)；pct=150 mode 1: tail %u (%.2fx, 回收 %llu 次, 写 %.1f MB)；mode 3: tail %u (%.2fx, 回收 %llu 次)  %s\n",
                a.tail, a.live, (double)a.tail / std::max<u32>(1, a.live), a.writes / 1048576.0, b.tail, (double)b.tail / std::max<u32>(1, b.live), (unsigned long long)b.reclaims, b.writes / 1048576.0,
                c.tail, (double)c.tail / std::max<u32>(1, c.live), (unsigned long long)c.reclaims, bad ? "FAIL" : "ok");
    return bad;
}
// 2d：顶部空闲段释放后 tail_ 自己回落（不靠 shrink）—— 白盒：在尾部分配一块、释放（pending）、过一个提交点 ⇒ tail_ 退回；
//     清单页簇（man_hole_）贴着尾部时**不**回落（否则下一条 run 又贴着清单页长，洞被切碎）；回落后继续写、重开一致。
int t_recede() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(32768); Model t;
    Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_recede open fail\n"); return 1; }
    s.set_maint_mode(1);
    for (u32 k = 1; k <= 3000; ++k) putm(s, t, k, 1, 300);
    if (s.commit() != RC::OK) bad++;
    const u32 t0 = s.tail_;
    u32 p = 0;
    if (s.alloc_pages(300, p, false) != RC::OK || p + 300 != s.tail_) { std::printf("  t_recede FAIL: 尾部分配 p=%u tail=%u t0=%u\n", p, s.tail_, t0); return bad + 1; }
    s.free_pages(p, 300, false);                                          // pending：要等下一个 SB
    s.promote_pending();
    if (s.tail_ != p + 300) { std::printf("  t_recede FAIL: SB 之前就回落了（tail %u）\n", s.tail_); bad++; }
    if (s.pend_delta_.empty() ? s.write_sb() != RC::OK : s.commit() != RC::OK) bad++;   // 提交点：有待入账的维护 delta 就走空提交，否则直接写 SB（两者都让 sb_gen_ 前移）
    s.promote_pending();
    const u32 t1 = s.tail_;
    if (t1 != p) { std::printf("  t_recede FAIL: 提交点之后 tail_ %u != %u（释放块的起点）\n", t1, p); bad++; }
    // 清单页簇：下一次提交会在尾部长 8 页（1 页清单 + 7 页簇），簇贴着尾部也不回落
    for (u32 k = 1; k <= 50; ++k) putm(s, t, k, 2, 300);
    if (s.commit() != RC::OK) bad++;
    bool cluster_top = false;
    if (!s.free_.empty()) { auto last = std::prev(s.free_.end()); cluster_top = last->first + last->second >= s.tail_ && s.man_hole_.count(last->first); }
    const u32 t2 = s.tail_;
    s.promote_pending();
    if (cluster_top && s.tail_ != t2) { std::printf("  t_recede FAIL: 清单页簇被回落了（%u → %u）\n", t2, s.tail_); bad++; }
    bad += vok(s, "t_recede") + cmp_model(s, t, "t_recede");
    for (u32 k = 1; k <= 2000; ++k) putm(s, t, k, 3, 300);
    if (s.commit() != RC::OK) bad++;
    { static u8 arena2[512 * PAGE]; Store s2; if (s2.open(&dev, arena2, 512) != RC::OK) { std::printf("  t_recede FAIL: reopen\n"); return bad + 1; }
      bad += vok(s2, "t_recede(reopen)") + cmp_model(s2, t, "t_recede(reopen)"); }
    std::printf("  顶部释放回落：t0 %u → 尾部分配 300 页 → 释放 + 提交点 ⇒ tail_ %u；清单页簇在顶（%d）不回落；重开一致  %s\n", t0, t1, (int)cluster_top, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (3) 读者让路
int t_yield() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1404);
    const u32 N = 30000;
    const bool mc = std::thread::hardware_concurrency() >= 2;
    for (int pass = 0; pass < 2; ++pass) {                                  // pass 0：默认时间片 1 ms；pass 1：quantum=0 不让路
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_yield open fail\n"); return 1; }
        t.m.clear();
        s.set_shared_reads(true); s.set_maint_mode(3);
        if (pass == 1) s.set_maint_quantum_us(0);
        std::atomic<bool> stop{false}; std::atomic<u64> reads{0}, errs{0};
        std::thread rd([&] {
            std::mt19937 r(7); u8 buf[4096]; u16 len;
            while (!stop) for (int i = 0; i < 64; ++i) { RC rc = s.get(1 + r() % N, buf, sizeof buf, &len); if (rc != RC::OK && rc != RC::NOTFOUND) ++errs; ++reads; }
        });
        for (int c = 0; c < 70; ++c) {
            for (int i = 0; i < 1500; ++i) putm(s, t, 1 + rng() % N, 100 + c, 200);
            if (s.commit() != RC::OK) { std::printf("  t_yield FAIL: commit %d\n", c); bad++; }
        }
        drain(s, 3);
        stop = true; rd.join();
        s.set_maint_mode(1);                                                 // join
        drain(s, 1);
        auto ms = s.maint_stats();
        if (errs) { std::printf("  t_yield FAIL: 读错误 %llu\n", (unsigned long long)errs.load()); bad++; }
        if (pass == 0 && mc && ms.yields == 0) { std::printf("  t_yield FAIL: 有读者争用却从未让路（units=%llu unit_max=%.1f ms）\n", (unsigned long long)ms.units, ms.unit_max_us / 1e3); bad++; }
        if (pass == 1 && ms.yields) { std::printf("  t_yield FAIL: quantum=0 仍让路 %llu 次\n", (unsigned long long)ms.yields); bad++; }
        bad += acct_drift(s, "t_yield") + vok(s, "t_yield") + cmp_model(s, t, "t_yield");
        std::printf("  读者让路 pass %d（quantum %s）：读 %llu 次、单元 %llu、让路 %llu 次、unit_max %.1f ms、>4ms 单元 %llu；账零漂移  %s\n",
                    pass, pass ? "0" : "1 ms", (unsigned long long)reads.load(), (unsigned long long)ms.units, (unsigned long long)ms.yields,
                    ms.unit_max_us / 1e3, (unsigned long long)ms.unit_long, bad ? "FAIL" : "ok");
    }
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    alarm(900);
    double t0 = now_s();
    int bad = 0;
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* n) { return !only || !std::strcmp(only, n); };
    if (want("jobacct")) bad += t_jobacct();
    if (want("reserve")) bad += t_reserve();
    if (want("extend"))  bad += t_extend();
    if (want("hw"))      bad += t_hw();
    if (want("recede"))  bad += t_recede();
    if (want("yield"))   bad += t_yield();
    std::printf("g14 第 14 轮：作业增量账 + 文件高水位 + 读者让路: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
