// g15 —— 第 15 轮回归：高水位回收的**二步搬家**（hw_reclaim_pass / hw2_plan）。
//   白盒搭一个第 14 轮策略会卡死的布局：顶部 run 装不进任何一个洞，但几个洞之间只隔着小 run：
//     [簇][F0 洞][F1 洞][R2a][F2 洞][R2b][F3 洞][R3 顶部大 run]      F1+R2a+F2 < need，F1..F3 全段 ≥ need ⇒ 要搬开 R2a、R2b 两条
//   判据：(a) 搬之前确实没有洞装得下顶部 run（老策略会 return false）；(b) 每个提交点最多搬开一条挡路 run；
//         (c) 搬开的 run 落在窗之外；(d) 顶部 run 最终搬进了窗、tail_ 回到阈值以内；(e) 快照钉住挡路 run 时它是墙（不搬、不坏）；
//         (f) 默认 pct=0 不动；(g) 全程 verify + 真值 + 账零漂移 + 重开一致。单跑 < 2 s（RamDev）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <unistd.h>
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
void drain(Store& s) {
    double t0 = now_s();
    while (s.maint_pending() && now_s() - t0 < 60) s.commit();
    for (int i = 0; i < 64 && s.maintain(8) == RC::MORE; ++i) {}
}
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
            if (bad++ < 3) std::printf("  %s FAIL: 账漂移 cell=[%llu,%llu) live %llu vs %llu dead %llu vs %llu\n", tag,
                                       (unsigned long long)c->lo, (unsigned long long)c->hi, (unsigned long long)l0, (unsigned long long)c->live,
                                       (unsigned long long)d0, (unsigned long long)c->dead);
        }
        ++checked;
    }
    if (!checked) { std::printf("  %s FAIL: 没有一个已知单元可对账（未知 %u）\n", tag, unknown); return 1; }
    return bad ? 1 : 0;
}
void dump(Store& s, const char* tag) {
    std::printf("  [%s] tail=%u live=%llu pending=%zu\n", tag, s.tail_, (unsigned long long)s.live_page_count(), s.pending_free_.size());
    std::vector<std::pair<u32, Run*>> rs; for (auto& kv : s.runs_) rs.push_back({kv.first, kv.second}); std::sort(rs.begin(), rs.end());
    u32 prev = 2;
    for (auto& pr : rs) { u32 blk = s.run_block(pr.second); if (pr.first > prev) std::printf("     [gap %u]\n", pr.first - prev); std::printf("     run %u blk %u L%u ver %llu\n", pr.first, blk, pr.second->lvl, (unsigned long long)pr.second->ver); prev = pr.first + blk; }
    if (s.tail_ > prev) std::printf("     [gap %u to tail]\n", s.tail_ - prev);
    std::printf("     free:"); for (auto& kv : s.free_) if (kv.first < s.tail_) std::printf(" [%u,+%u)", kv.first, kv.second); std::printf("\n");
}
// 顶部 run（page0 最大）
Run* top_run(Store& s) { Run* top = nullptr; for (auto& kv : s.runs_) if (kv.second->npages && (!top || kv.second->page0 > top->page0)) top = kv.second; return top; }
// 一批键写成一条 run（lvl_t 很大 ⇒ 不会被层合并动）；返回这条 run
Run* flush_batch(Store& s, Model& t, u64 k0, u32 n, u32 tag, int* bad) {
    std::vector<u32> before; for (auto& kv : s.runs_) before.push_back(kv.first);
    for (u32 i = 0; i < n; ++i) putm(s, t, k0 + i, tag, 200);
    if (s.commit() != RC::OK) { std::printf("  flush_batch FAIL: commit\n"); (*bad)++; return nullptr; }
    Run* nr = nullptr; u32 cnt = 0;
    for (auto& kv : s.runs_) if (std::find(before.begin(), before.end(), kv.first) == before.end()) { nr = kv.second; ++cnt; }
    if (cnt != 1) { std::printf("  flush_batch FAIL: 一批写出了 %u 条 run（期望 1）\n", cnt); (*bad)++; }
    return nr;
}
struct Fill { u32 p0, n; };
Fill fill(Store& s, u32 n, int* bad) { u32 p = 0; if (s.alloc_pages(n, p, true) != RC::OK) { std::printf("  fill FAIL: alloc %u\n", n); (*bad)++; } return Fill{p, n}; }

// 搭布局。snap_between = 1 ⇒ 在 R2a/R2b 之后、R3 之前抓快照（R2a/R2b 被钉住、R3 不被钉）。返回窗的范围 [wlo, whi) 与各 run。
// 注意：run 搬家后旧的 Run 对象就没了（指针悬空），身份只能靠 seq（搬家不改 seq / ver）。
struct Scene { Run *r2a = nullptr, *r2b = nullptr, *r3 = nullptr; u64 s2a = 0, s2b = 0, s3 = 0; u32 wlo = 0, whi = 0, tail0 = 0; Fill f0{0, 0}, wall{0, 0}; };
// run 布局的顶 = 最高 run 块的末尾。策略只管 run；tail_ 还可能被一个短命的清单页 / 清单页簇钉着（t_recede 测的就是簇不回落），
// 所以阈值判据用它而不是 tail_。
u32 runs_top(Store& s) { u32 e = 2; for (auto& kv : s.runs_) if (kv.second->npages) e = std::max(e, kv.second->page0 + s.run_block(kv.second)); return e; }
Run* by_seq(Store& s, u64 seq) { for (auto& kv : s.runs_) if (kv.second->seq == seq) return kv.second; return nullptr; }
Scene build(Store& s, Model& t, int snap_between, int* bad) {
    Scene sc;
    s.set_maint_mode(1); s.set_lvl_t(1000); s.set_merge_budget_pages(64);
    for (u32 i = 0; i < 10; ++i) putm(s, t, 1000000 + i, 9, 200);
    if (s.commit() != RC::OK) (*bad)++;                                  // 先把清单页簇（8 页）落在最低处：之后的清单页都从簇里取，不会切开窗
    sc.f0 = fill(s, 60, bad);                                            // 挡路 run 的目的地（窗外）
    sc.wall = fill(s, 4, bad);                                           // 一直占着的 4 页当墙：F0 和 F1 释放后不能并成一个洞（否则最便宜的窗就变了；
                                                                         // 一条 1 页的小 run 当不了墙——它块 < 顶部块、规划器会把它当挡路 run 搬走）。测完再还、再 verify
    Fill f1 = fill(s, 80, bad);
    sc.r2a = flush_batch(s, t, 10000, 400, 1, bad);                      // ≈ 20 页
    Fill f2 = fill(s, 80, bad);
    sc.r2b = flush_batch(s, t, 20000, 400, 2, bad);
    Fill f3 = fill(s, 80, bad);
    if (snap_between) {                                                  // 快照之前先把 F0..F2 放回去（快照存活期间释放的页进 deferred_、出不来）；
        for (Fill f : {sc.f0, f1, f2}) s.free_pages(f.p0, f.n);         // F3 留着：R3 要落在 F3 之上（否则 R3 会从 F3 起贴着尾部长、F3 就没了）
        if (s.commit() != RC::OK) (*bad)++;
        if (s.snapshot() != RC::OK) { std::printf("  build FAIL: snapshot\n"); (*bad)++; }
    }
    sc.r3 = flush_batch(s, t, 30000, 4000, 3, bad);                      // ≈ 200 页：顶部大 run
    if (!snap_between) { for (Fill f : {sc.f0, f1, f2, f3}) s.free_pages(f.p0, f.n); }
    else s.free_pages(f3.p0, f3.n);                                      // 快照存活 ⇒ 进 deferred_，释放快照后才成洞
    if (sc.r2a) sc.s2a = sc.r2a->seq;
    if (sc.r2b) sc.s2b = sc.r2b->seq;
    if (sc.r3) sc.s3 = sc.r3->seq;
    sc.wlo = f1.p0; sc.whi = f3.p0 + f3.n; sc.tail0 = s.tail_;
    return sc;
}

int t_twostep() {
    int bad = 0; static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_twostep open fail\n"); return 1; }
    Scene sc = build(s, t, 0, &bad);
    if (bad || !sc.r2a || !sc.r2b || !sc.r3) { dump(s, "build"); return bad + 1; }
    const u32 r3p0 = sc.r3->page0, r3blk = s.run_block(sc.r3);
    const u32 r2ap0 = sc.r2a->page0, r2bp0 = sc.r2b->page0;
    // 先过一个提交点让填充块真正变成洞（策略还没开：pct=0）
    if (s.commit() != RC::OK) bad++;
    if (s.maint_stats().hw_reclaims || s.maint_stats().hw_blockers) { std::printf("  t_twostep FAIL: pct=0 却动了（reclaims=%llu blockers=%llu）\n", (unsigned long long)s.maint_stats().hw_reclaims, (unsigned long long)s.maint_stats().hw_blockers); bad++; }
    const u64 live = s.live_page_count();
    if ((u64)runs_top(s) * 100 <= live * 150) { std::printf("  t_twostep FAIL: 布局没超阈值（runs_top %u tail %u live %llu）\n", runs_top(s), s.tail_, (unsigned long long)live); bad++; }
    if (s.hole_below(r3p0, (u64)r3blk + 4, 0, 0)) { std::printf("  t_twostep FAIL: 顶部 run 本来就有洞装（老策略不会卡）\n"); dump(s, "pre"); bad++; }
    if (top_run(s) != sc.r3) { std::printf("  t_twostep FAIL: 顶部不是 R3\n"); bad++; }
    if (bad) { dump(s, "pre"); return bad; }
    // 开策略：逐提交点盯 blockers 的增量（≤ 1 / 提交点）
    s.set_hw_reclaim_pct(150);
    u64 pb = 0; int steps = 0;
    for (; steps < 20; ++steps) {
        if (s.commit() != RC::OK) { bad++; break; }
        auto ms = s.maint_stats();
        if (ms.hw_blockers > pb + 1) { std::printf("  t_twostep FAIL: 一个提交点搬开了 %llu 条挡路 run\n", (unsigned long long)(ms.hw_blockers - pb)); bad++; }
        pb = ms.hw_blockers;
        if ((u64)runs_top(s) * 100 <= s.live_page_count() * 150 && !s.job_ && !s.maint_pending() && s.pending_free_.empty()) break;
    }
    drain(s);
    for (int i = 0; i < 3; ++i) if (s.commit() != RC::OK) bad++;         // 最后一次搬家的源块过提交点 ⇒ tail_ 回落
    auto ms = s.maint_stats();
    if (ms.hw_blockers != 2) { std::printf("  t_twostep FAIL: 挡路 run 搬开次数 %llu（期望 2：R2a、R2b 各一次）\n", (unsigned long long)ms.hw_blockers); bad++; }
    if (ms.hw_reclaims != 1) { std::printf("  t_twostep FAIL: 顶部搬家次数 %llu（期望 1）\n", (unsigned long long)ms.hw_reclaims); bad++; }
    if ((u64)runs_top(s) * 100 > s.live_page_count() * 150) { std::printf("  t_twostep FAIL: run 布局顶 %u（tail %u）没回到 1.5× 活页 %llu 以内\n", runs_top(s), s.tail_, (unsigned long long)s.live_page_count()); bad++; }
    // 三条 run 现在在哪：R3 在窗里、R2a/R2b 在窗外（run 的身份 = 同 seq：搬家后 page0 变了、旧 Run 对象已销毁）
    Run *n2a = by_seq(s, sc.s2a), *n2b = by_seq(s, sc.s2b), *n3 = by_seq(s, sc.s3);
    (void)r2ap0; (void)r2bp0;
    if (!n2a || !n2b || !n3) { std::printf("  t_twostep FAIL: 搬家后按 seq 找不到 run\n"); bad++; }
    else {
        if (n3->page0 >= r3p0) { std::printf("  t_twostep FAIL: 顶部 run 没有下移（%u → %u）\n", r3p0, n3->page0); bad++; }
        if (!(n3->page0 >= sc.wlo && n3->page0 + s.run_block(n3) <= sc.whi)) { std::printf("  t_twostep FAIL: 顶部 run 没落在并出来的窗 [%u,%u) 里（page0=%u blk=%u）\n", sc.wlo, sc.whi, n3->page0, s.run_block(n3)); bad++; }
        for (Run* r : {n2a, n2b}) if (r->page0 < sc.whi && r->page0 + s.run_block(r) > sc.wlo) { std::printf("  t_twostep FAIL: 挡路 run 搬回了窗里（page0=%u）\n", r->page0); bad++; }
    }
    if (bad) dump(s, "post");
    const u32 rt = runs_top(s);
    s.free_pages(sc.wall.p0, sc.wall.n);                                  // 墙还回去（否则 verify 报 page unowned）
    for (int i = 0; i < 2; ++i) if (s.commit() != RC::OK) bad++;
    bad += acct_drift(s, "t_twostep") + vok(s, "t_twostep") + cmp_model(s, t, "t_twostep");
    { static u8 arena2[1024 * PAGE]; Store s2; if (s2.open(&dev, arena2, 1024) != RC::OK) { std::printf("  t_twostep FAIL: reopen\n"); return bad + 1; }
      bad += vok(s2, "t_twostep(reopen)") + cmp_model(s2, t, "t_twostep(reopen)"); }
    std::printf("  二步搬家：run 布局顶 %u → %u（tail %u，活页 %llu，%.2fx），搬开挡路 run %llu 条、顶部搬家 %llu 次、%d 个提交点  %s\n", sc.tail0, rt, s.tail_,
                (unsigned long long)s.live_page_count(), (double)rt / std::max<u64>(1, s.live_page_count()), (unsigned long long)ms.hw_blockers, (unsigned long long)ms.hw_reclaims, steps + 1, bad ? "FAIL" : "ok");
    return bad;
}

// (e) 快照钉住挡路 run ⇒ 墙：策略开着也不动；释放快照后照常二步搬家
int t_pinned() {
    int bad = 0; static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_pinned open fail\n"); return 1; }
    Scene sc = build(s, t, 1, &bad);
    if (bad || !sc.r2a || !sc.r2b || !sc.r3) { dump(s, "build"); return bad + 1; }
    const u32 r3p0 = sc.r3->page0, r3blk = s.run_block(sc.r3);
    if (!(s.nsnap_ > 0 && sc.r2a->ver <= s.snap_max() && sc.r3->ver > s.snap_max())) { std::printf("  t_pinned FAIL: 快照没有按预期钉住 R2a 而放过 R3（nsnap=%u r2a.ver=%llu r3.ver=%llu snap_max=%llu）\n", s.nsnap_, (unsigned long long)sc.r2a->ver, (unsigned long long)sc.r3->ver, (unsigned long long)s.snap_max()); bad++; }
    if (s.commit() != RC::OK) bad++;
    if (s.hole_below(r3p0, (u64)r3blk + 4, 0, 0)) { std::printf("  t_pinned FAIL: 顶部 run 本来就有洞装\n"); bad++; }
    if ((u64)runs_top(s) * 100 <= s.live_page_count() * 150) { std::printf("  t_pinned FAIL: 布局没超阈值（runs_top %u live %llu）\n", runs_top(s), (unsigned long long)s.live_page_count()); bad++; }
    if (bad) { dump(s, "pre"); return bad; }
    s.set_hw_reclaim_pct(150);
    for (int i = 0; i < 6; ++i) if (s.commit() != RC::OK) bad++;
    auto ms = s.maint_stats();
    if (ms.hw_blockers || ms.hw_reclaims) { std::printf("  t_pinned FAIL: 快照存活期间动了（blockers=%llu reclaims=%llu）\n", (unsigned long long)ms.hw_blockers, (unsigned long long)ms.hw_reclaims); bad++; }
    if (top_run(s) != sc.r3 || sc.r3->page0 != r3p0) { std::printf("  t_pinned FAIL: 顶部 run 被动过\n"); bad++; }
    bad += cmp_model(s, t, "t_pinned(snap)");
    if (bad) dump(s, "snap");
    if (s.forget_snapshots() != RC::OK) { std::printf("  t_pinned FAIL: forget_snapshots\n"); bad++; }
    for (int i = 0; i < 20; ++i) { if (s.commit() != RC::OK) { bad++; break; } if ((u64)runs_top(s) * 100 <= s.live_page_count() * 150 && !s.job_ && !s.maint_pending() && s.pending_free_.empty()) break; }
    drain(s);
    for (int i = 0; i < 3; ++i) if (s.commit() != RC::OK) bad++;
    ms = s.maint_stats();
    if (ms.hw_blockers < 1 || ms.hw_blockers > 2 || ms.hw_reclaims != 1) { std::printf("  t_pinned FAIL: 释放快照后 blockers=%llu reclaims=%llu（期望 1~2 / 1）\n", (unsigned long long)ms.hw_blockers, (unsigned long long)ms.hw_reclaims); bad++; }
    if ((u64)runs_top(s) * 100 > s.live_page_count() * 150) { std::printf("  t_pinned FAIL: 释放快照后 run 布局顶 %u（tail %u）没回落（活页 %llu）\n", runs_top(s), s.tail_, (unsigned long long)s.live_page_count()); bad++; }
    if (bad) dump(s, "post");
    const u32 rt = runs_top(s);
    s.free_pages(sc.wall.p0, sc.wall.n);
    for (int i = 0; i < 2; ++i) if (s.commit() != RC::OK) bad++;
    bad += acct_drift(s, "t_pinned") + vok(s, "t_pinned") + cmp_model(s, t, "t_pinned");
    std::printf("  快照钉住挡路 run：存活期间不动；释放后 run 布局顶 → %u（%.2fx）blockers=%llu reclaims=%llu  %s\n", rt, (double)rt / std::max<u64>(1, s.live_page_count()),
                (unsigned long long)ms.hw_blockers, (unsigned long long)ms.hw_reclaims, bad ? "FAIL" : "ok");
    return bad;
}

// (h) 规划器单元测试：直接喂 hw2_plan 一个布局，看它选的窗 / 挡路 run 是不是"搬走页数最少"的那个。
//     布局：[F1 80][R2a][F2 80][R2b][F3 80] 之外再放一条 [F4 30][R2c][F5 30]：R2c 所在的小窗装不下顶部 ⇒ 不该被选。
//     这里复用 t_twostep 的场景，只检查第一次规划的输出。
int t_plan() {
    int bad = 0; static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t;
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_plan open fail\n"); return 1; }
    Scene sc = build(s, t, 0, &bad);
    if (bad) return bad;
    if (s.commit() != RC::OK) bad++;                                     // 洞落地
    u32 wlo = 0, whi = 0;
    Run* b = s.hw2_plan(sc.r3, (u64)s.run_block(sc.r3) + 4, wlo, whi);
    if (!b) { std::printf("  t_plan FAIL: 规划器没找到窗\n"); dump(s, "plan"); return bad + 1; }
    if (b != sc.r2a && b != sc.r2b) { std::printf("  t_plan FAIL: 选的挡路 run 不是 R2a/R2b（page0=%u）\n", b->page0); bad++; }
    if (wlo != sc.wlo || whi != sc.whi) { std::printf("  t_plan FAIL: 窗 [%u,%u) ≠ 期望 [%u,%u)\n", wlo, whi, sc.wlo, sc.whi); bad++; }
    // 窗内有洞但没有窗外目的地 ⇒ 规划器必须放弃（把 F0 占掉）
    u32 p = 0; if (s.alloc_pages(sc.f0.n, p, true) != RC::OK || p != sc.f0.p0) { std::printf("  t_plan FAIL: 重新占 F0 失败（p=%u 期望 %u）\n", p, sc.f0.p0); bad++; }
    Run* b2 = s.hw2_plan(sc.r3, (u64)s.run_block(sc.r3) + 4, wlo, whi);
    if (b2) { std::printf("  t_plan FAIL: 窗外没有目的地却还选了挡路 run %u\n", b2->page0); bad++; }
    s.free_pages(p, sc.f0.n);
    s.free_pages(sc.wall.p0, sc.wall.n);
    if (s.commit() != RC::OK) bad++;
    if (bad) dump(s, "plan");
    bad += vok(s, "t_plan") + cmp_model(s, t, "t_plan");
    std::printf("  规划器：窗 [%u,%u) 挡路 run page0=%u；无窗外目的地时放弃  %s\n", sc.wlo, sc.whi, b->page0, bad ? "FAIL" : "ok");
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    auto want = [&](const char* n) { if (argc < 2) return true; for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], n)) return true; return false; };
    int bad = 0;
    std::printf("g15 第 15 轮：高水位回收二步搬家\n");
    if (want("plan"))    bad += t_plan();
    if (want("twostep")) bad += t_twostep();
    if (want("pinned"))  bad += t_pinned();
    std::printf(bad ? "g15 FAIL (%d)\n" : "g15 OK\n", bad);
    return bad ? 1 : 0;
}
