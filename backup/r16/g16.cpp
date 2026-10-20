// g16 —— 第 16 轮回归：P2b 分片归并 + RUN_TRIM。
//   (1) 分片基本面：作业每攒够一片就落地（merge_parts > 0、run_trims > 0），全程 verify / 与真值比对零差；清账后账零漂移；
//       合并时刻的峰值（tail 增长）≤ 约两片 + 预留（而不是整个产物）。
//   (2) 裁过的源 + 片产物跨重开：作业中途 close/open（作业作废、裁源与片留在清单里）⇒ 重放 RUN_TRIM、所有权（verify 认领）、
//       读正确；继续写 + 维护把家族并掉。
//   (3) 回退：分片落地之后 rollback() ⇒ 裁源 / 片全部回到上一个提交点，verify + 比对。
//   (4) 快照钉住：作业中途开快照 ⇒ 作业作废；快照视图读正确（get_at），放掉快照后维护继续。
//   (5) 溢出链：大值（溢出链）负载下分片——接管规则（第 0 页 / 边界页不接管）、裁剪时归还未接管链、重开后按附件页过滤 ⇒
//       verify 无 "owned twice / unowned / ovfl data"。
//   (6) 墓碑不复活：作业作废后留下"裁过的源 + 同 seq 的片在上一层"，再删这些键、再合并 ⇒ 仍 NOTFOUND（older_may_contain_grp）。
// 判据：每段 verify + 与真值比对；失败打印第一处。单跑 ≈ 10 ~ 30 s（RamDev）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
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
int cmp_model(Store& s, const Model& t, const char* tag, u64 view = ~0ull) {
    static u8 buf[65536]; u16 len = 0; int bad = 0;
    for (auto& kv : t.m) {
        RC rc = view == ~0ull ? s.get(kv.first, buf, 65535, &len) : s.get_at(kv.first, view, buf, 65535, &len);
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
    int bad = 0; u32 checked = 0;
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
        ++checked;
    }
    return bad ? 1 : 0;
}
u32 trimmed_runs(Store& s) { u32 n = 0; for (auto& kv : s.runs_) if (kv.second->trim || kv.second->first != kv.second->first0) ++n; return n; }
u32 live_pages(Store& s) { const u64 f = s.free_page_count(); return s.tail_ > f ? (u32)(s.tail_ - f) : 0; }

// ------------------------------------------------------------------ (1) 分片基本面
int t_parts() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1601);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_parts open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_maint_budget_us(20000); s.set_merge_budget_pages(32); s.set_merge_part_pages(64); s.set_lvl_t(4);
    const u32 N = 30000;
    u32 peak_over = 0; u32 max_trim = 0; u32 max_parts_seen = 0;
    for (int c = 0; c < 150; ++c) {
        for (int i = 0; i < 600; ++i) putm(s, t, 1 + rng() % N, 100 + c, 300);
        if (c % 7 == 3) for (int i = 0; i < 20; ++i) delm(s, t, 1 + rng() % N);
        if (c % 29 == 11) { u64 lo = 1 + rng() % N; u64 hi = std::min<u64>(N + 1, lo + 150); s.del_range(lo, hi); for (u64 k = lo; k < hi; ++k) if (t.m.count(k)) t.m[k] = 0; }
        if (s.commit() != RC::OK) { std::printf("  t_parts FAIL: commit %d\n", c); bad++; }
        const u32 lp = live_pages(s);
        if (s.tail_ > lp) peak_over = std::max(peak_over, s.tail_ - lp);
        max_trim = std::max(max_trim, trimmed_runs(s));
        if (c % 30 == 29) { bad += cmp_model(s, t, "t_parts"); bad += vok(s, "t_parts"); }
    }
    max_parts_seen = (u32)s.merge_parts();
    drain(s);
    if (s.merge_parts() == 0) { std::printf("  t_parts FAIL: 从未分片落地（merge_parts=0）\n"); bad++; }
    if (s.run_trims() == 0) { std::printf("  t_parts FAIL: 从未写 RUN_TRIM\n"); bad++; }
    if (trimmed_runs(s)) { std::printf("  t_parts FAIL: 清账后仍有裁过的源 run %u 条（家族没并掉）\n", trimmed_runs(s)); bad++; }
    if (s.unk_cells_) { std::printf("  t_parts FAIL: 清账后仍有未知单元 %u\n", s.unk_cells_); bad++; }
    bad += acct_drift(s, "t_parts") + vok(s, "t_parts") + cmp_model(s, t, "t_parts");
    auto ms = s.maint_stats();
    std::printf("  分片基本面：150 次提交 parts=%llu trims=%llu job_fast=%llu job_slow=%llu；tail 超出活页峰值 %u 页；清账后 runs=%u  %s\n",
                (unsigned long long)max_parts_seen, (unsigned long long)s.run_trims(), (unsigned long long)ms.job_fast, (unsigned long long)ms.job_slow,
                peak_over, s.run_count(), bad ? "FAIL" : "ok");
    return bad;
}

// 搭一个"作业跑到一半、已经封了几片"的状态：返回时 s.job_ 非空且至少 1 片已落地、至少一条源裁过
// uniq = true：键单调递增、每个键只写一次（50000 起）⇒ 片里的键在更老的 run 里**没有**副本（墓碑测试要这个前提）
int build_midjob(Store& s, Model& t, std::mt19937_64& rng, const char* tag, u32 vlen = 300, bool uniq = false) {
    s.set_maint_mode(1); s.set_maint_budget_us(2000); s.set_merge_budget_pages(16); s.set_merge_part_pages(48); s.set_lvl_t(4); s.set_maint_units_max(4);
    const u32 N = 20000;
    u64 nextk = 50000;
    for (int c = 0; c < 400; ++c) {
        for (int i = 0; i < 400; ++i) putm(s, t, uniq ? nextk++ : 1 + rng() % N, 100 + c, vlen);
        if (!uniq && c % 5 == 2) for (int i = 0; i < 10; ++i) delm(s, t, 1 + rng() % N);
        if (s.commit() != RC::OK) { std::printf("  %s FAIL: commit %d\n", tag, c); return 1; }
        if (s.job_ && s.merge_parts() >= 2 && trimmed_runs(s) >= 1) return 0;
    }
    std::printf("  %s FAIL: 400 次提交也没等到\"作业中途 + 已封片\"的状态（parts=%llu trimmed=%u job=%d）\n", tag, (unsigned long long)s.merge_parts(), trimmed_runs(s), s.job_ ? 1 : 0);
    return 1;
}

// ------------------------------------------------------------------ (2) 跨重开
int t_reopen() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1602);
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_reopen open fail\n"); return 1; }
        if (build_midjob(s, t, rng, "t_reopen")) return 1;
        // 作业中途：裁源 + 片都在内存与清单里；先验证这一刻
        bad += vok(s, "t_reopen@mid") + cmp_model(s, t, "t_reopen@mid");
        // 把增量落盘（提交点），然后 close（作业作废）
        if (s.commit() != RC::OK) bad++;
    }                                                   // 析构 = 关库（作业作废、未落盘的维护增量丢弃）
    u32 tr_after = 0;
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_reopen FAIL: reopen\n"); return bad + 1; }
        s.set_maint_mode(1); s.set_merge_budget_pages(16); s.set_merge_part_pages(48); s.set_lvl_t(4);
        tr_after = trimmed_runs(s);
        if (!tr_after) { std::printf("  t_reopen FAIL: 重开后没有裁过的源 run（TRIM 没重放？）\n"); bad++; }
        // 裁过的 run：first 必须 > first0、trim 页在数据页内、nbytes 已扣
        for (auto& kv : s.runs_) {
            RunPtr r = kv.second;
            if (!r->trim && r->first == r->first0) continue;
            s.ensure_meta(r);
            if (r->trim >= r->npages || r->first < r->first0 || r->nbytes > r->nbytes0 || (r->trim && r->fences[r->trim] > r->first)) {
                std::printf("  t_reopen FAIL: 裁剪状态不自洽 rid=%u trim=%u npg=%u first=%llu first0=%llu nbytes=%u/%u\n", r->page0, r->trim, r->npages,
                            (unsigned long long)r->first, (unsigned long long)r->first0, r->nbytes, r->nbytes0);
                bad++; break;
            }
        }
        bad += vok(s, "t_reopen@open") + cmp_model(s, t, "t_reopen@open");
        // 继续写 + 清账：家族要被并掉，裁源要消失
        for (int c = 0; c < 60; ++c) {
            for (int i = 0; i < 300; ++i) putm(s, t, 1 + rng() % 20000, 900 + c, 300);
            if (s.commit() != RC::OK) bad++;
        }
        drain(s);
        bad += acct_drift(s, "t_reopen") + vok(s, "t_reopen@end") + cmp_model(s, t, "t_reopen@end");
        if (trimmed_runs(s)) { std::printf("  t_reopen FAIL: 清账后仍有裁过的源 %u 条\n", trimmed_runs(s)); bad++; }
        std::printf("  跨重开：作业中途关库（重开后裁过的源 %u 条、runs=%u）⇒ 重放 TRIM、所有权、读全对；继续维护后家族并掉  %s\n", tr_after, s.run_count(), bad ? "FAIL" : "ok");
    }
    return bad;
}

// ------------------------------------------------------------------ (3) 回退
// 3a：abort()——分片中途、未提交的一批写丢掉（维护增量也随之按盘重建）；3b：rollback(快照)——整个分片过程回退到快照点。
int t_rollback() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1603);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_rollback open fail\n"); return 1; }
    // 先写一点、开快照（之后的 run 都比快照新 ⇒ 不被钉住，作业照跑）
    for (int i = 0; i < 2000; ++i) putm(s, t, 1 + rng() % 20000, 1, 300);
    if (s.commit() != RC::OK) bad++;
    if (s.snapshot() != RC::OK) { std::printf("  t_rollback FAIL: snapshot\n"); return bad + 1; }
    Model tsnap = t;
    if (build_midjob(s, t, rng, "t_rollback")) return 1;
    Model t0 = t;
    for (int i = 0; i < 500; ++i) putm(s, t, 1 + rng() % 20000, 7777, 300);
    if (s.abort() != RC::OK) { std::printf("  t_rollback FAIL: abort rc\n"); bad++; }
    t = t0;
    bad += vok(s, "t_rollback@abort") + cmp_model(s, t, "t_rollback@abort");
    // 再推进几步维护 + 提交，然后整体回退到快照
    for (int c = 0; c < 10; ++c) { for (int i = 0; i < 300; ++i) putm(s, t, 1 + rng() % 20000, 800 + c, 300); if (s.commit() != RC::OK) bad++; }
    if (s.rollback(0) != RC::OK) { std::printf("  t_rollback FAIL: rollback(0) rc\n"); bad++; }
    t = tsnap;
    bad += vok(s, "t_rollback@rb") + cmp_model(s, t, "t_rollback@rb");
    if (trimmed_runs(s)) { std::printf("  t_rollback FAIL: 回退到快照后仍有裁过的源 %u 条\n", trimmed_runs(s)); bad++; }
    s.forget_snapshots();
    for (int c = 0; c < 40; ++c) {
        for (int i = 0; i < 300; ++i) putm(s, t, 1 + rng() % 20000, 900 + c, 300);
        if (s.commit() != RC::OK) bad++;
    }
    drain(s);
    bad += acct_drift(s, "t_rollback") + vok(s, "t_rollback@end") + cmp_model(s, t, "t_rollback@end");
    std::printf("  回退：分片中途 abort()、再 rollback(快照) ⇒ 状态与清单一致（verify 所有权 + 比对），继续维护正常  %s\n", bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (4) 快照
int t_snapshot() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1604);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_snapshot open fail\n"); return 1; }
    if (build_midjob(s, t, rng, "t_snapshot")) return 1;
    u64 sv = 0;
    if (s.snapshot() != RC::OK) { std::printf("  t_snapshot FAIL: snapshot\n"); return bad + 1; }
    sv = s.snap_version(s.nsnap_ - 1);
    Model ts = t;
    // 快照期间继续写 + 维护：作业会因源被钉住而作废；裁源 / 片保持；快照视图必须不变
    for (int c = 0; c < 40; ++c) {
        for (int i = 0; i < 300; ++i) putm(s, t, 1 + rng() % 20000, 600 + c, 300);
        if (c % 4 == 1) for (int i = 0; i < 10; ++i) delm(s, t, 1 + rng() % 20000);
        if (s.commit() != RC::OK) bad++;
        if (c % 10 == 9) { bad += cmp_model(s, ts, "t_snapshot@snap", sv); bad += cmp_model(s, t, "t_snapshot@cur"); }
    }
    bad += vok(s, "t_snapshot@held");
    s.forget_snapshots();
    drain(s);
    bad += acct_drift(s, "t_snapshot") + vok(s, "t_snapshot@end") + cmp_model(s, t, "t_snapshot@end");
    std::printf("  快照：分片中途开快照（视图 %llu）⇒ 作业作废、快照读不变；放掉后维护收敛  %s\n", (unsigned long long)sv, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (5) 溢出链
int t_ovfl() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1605);
    const u32 N = 6000;
    u64 parts = 0;
    {
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_ovfl open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_maint_budget_us(20000); s.set_merge_budget_pages(32); s.set_merge_part_pages(64); s.set_lvl_t(4);
    for (int c = 0; c < 90; ++c) {
        for (int i = 0; i < 120; ++i) { const u64 k = 1 + rng() % N; putm(s, t, k, 100 + c, (k % 3 == 0) ? 6000 : 300); }   // 1/3 的键是溢出值
        if (c % 6 == 2) for (int i = 0; i < 10; ++i) delm(s, t, 1 + rng() % N);
        if (s.commit() != RC::OK) { std::printf("  t_ovfl FAIL: commit %d\n", c); bad++; }
        if (c % 30 == 29) { bad += cmp_model(s, t, "t_ovfl"); bad += vok(s, "t_ovfl"); }
    }
    parts = s.merge_parts();
    if (s.commit() != RC::OK) bad++;
    }
    Store s2; if (s2.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_ovfl FAIL: reopen\n"); return bad + 1; }
    s2.set_maint_mode(1); s2.set_merge_budget_pages(32); s2.set_merge_part_pages(64); s2.set_lvl_t(4);
    bad += vok(s2, "t_ovfl@open") + cmp_model(s2, t, "t_ovfl@open");
    for (int c = 0; c < 30; ++c) {
        for (int i = 0; i < 120; ++i) { const u64 k = 1 + rng() % N; putm(s2, t, k, 500 + c, (k % 3 == 0) ? 6000 : 300); }
        if (s2.commit() != RC::OK) bad++;
    }
    drain(s2);
    bad += acct_drift(s2, "t_ovfl") + vok(s2, "t_ovfl@end") + cmp_model(s2, t, "t_ovfl@end");
    std::printf("  溢出链：1/3 大值负载 parts=%llu；重开（附件页过滤）+ 继续维护 ⇒ verify 无所有权错误  %s\n", (unsigned long long)parts, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (6) 墓碑不复活
int t_tomb() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(1606);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_tomb open fail\n"); return 1; }
    if (build_midjob(s, t, rng, "t_tomb", 300, true)) return 1;
    // 作废作业（快照钉住一次再放掉），留下裁过的源（lvl）+ 同 seq 的片（lvl+1）
    if (s.snapshot() != RC::OK) bad++;
    for (int i = 0; i < 300; ++i) putm(s, t, 1 + rng() % 20000, 555, 300);
    if (s.commit() != RC::OK) bad++;
    s.forget_snapshots();
    if (s.job_) { std::printf("  t_tomb FAIL: 快照没把作业作废\n"); bad++; }
    // 片产物的键域 = 各裁源 [first0, first)：挑这些键删掉（墓碑进新 L0），再让层合并跑完
    std::vector<u64> victims;
    for (auto& kv : s.runs_) {
        RunPtr r = kv.second;
        if (r->first == r->first0) continue;
        for (u64 k = r->first0; k < r->first && victims.size() < 400; k += 1 + (r->first - r->first0) / 200) if (t.m.count(k) && t.m[k]) victims.push_back(k);
    }
    if (victims.empty()) { std::printf("  t_tomb FAIL: 找不到片键域里的存活键\n"); return bad + 1; }
    std::vector<char> isv(400001, 0);
    for (u64 k : victims) { delm(s, t, k); if (k < isv.size()) isv[k] = 1; }
    s.set_lvl_t(8);                                      // 让墓碑 run 和裁过的源进**同一组**（组的最小 seq < 片的 seq：老规则看不到片）
    if (s.commit() != RC::OK) bad++;
    for (int c = 0; c < 80; ++c) {                       // 之后不再写这些键：末了它们必须还是 NOTFOUND
        for (int i = 0; i < 200; ++i) { u64 k = 1 + rng() % 20000; putm(s, t, k, 700 + c, 300); }   // 1..20000 与受害键（50000+）不相交
        if (s.commit() != RC::OK) bad++;
        if (c % 20 == 19) bad += cmp_model(s, t, "t_tomb");
    }
    drain(s);
    int res = 0; static u8 buf[65536]; u16 len = 0;
    for (u64 k : victims) if (s.get(k, buf, 65535, &len) != RC::NOTFOUND) ++res;
    if (res) {
        std::printf("  t_tomb FAIL: %d/%zu 个被删的键复活\n", res, victims.size()); bad++;
        int shown = 0;
        for (u64 k : victims) {
            if (s.get(k, buf, 65535, &len) == RC::NOTFOUND || shown++ >= 3) continue;
            std::printf("    key %llu:\n", (unsigned long long)k);
            for (auto& kv : s.runs_) {
                RunPtr r = kv.second; const u8* v = nullptr; u16 l = 0;
                if (k < r->first0 || k > r->last) continue;
                RC rc = s.probe(r, r->trim, r->npages, k, ~0ull, v, l);
                std::printf("      run rid=%u seq=%llu lvl=%u ver=%llu trim=%u first0=%llu first=%llu last=%llu npg=%u -> %s len=%u\n", r->page0,
                            (unsigned long long)r->seq, r->lvl, (unsigned long long)r->ver, r->trim, (unsigned long long)r->first0, (unsigned long long)r->first,
                            (unsigned long long)r->last, r->npages, rc_str(rc), l);
            }
            Cell* c = s.find_cell(k);
            if (c) for (auto& sg : c->segs) std::printf("      seg rid=%u seq=%llu p=[%u,%u) k=[%llu,%llu) live=%u\n", sg.second.rid, (unsigned long long)sg.first.seq, sg.second.p0, sg.second.p1, (unsigned long long)sg.second.klo, (unsigned long long)sg.second.khi, sg.second.live);
        }
    }
    bad += vok(s, "t_tomb@end") + cmp_model(s, t, "t_tomb@end");
    std::printf("  墓碑：作业作废后删掉片键域里的 %zu 个键、再合并 ⇒ 复活 %d 个  %s\n", victims.size(), res, bad ? "FAIL" : "ok");
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* n) { return !only || std::strcmp(only, n) == 0; };
    int bad = 0; double t0 = now_s();
    if (want("parts")) bad += t_parts();
    if (want("reopen")) bad += t_reopen();
    if (want("rollback")) bad += t_rollback();
    if (want("snapshot")) bad += t_snapshot();
    if (want("ovfl")) bad += t_ovfl();
    if (want("tomb")) bad += t_tomb();
    std::printf("g16 第 16 轮：P2b 分片归并 + RUN_TRIM（分片 / 重开 / 回退 / 快照 / 溢出链 / 墓碑）: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
