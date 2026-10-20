// g18 —— 养洞护航 + 整理状态机 + 碎片感知切片（养洞合并 A）
//   (1) t_protect：中部大洞对用户 alloc 跳过、对流式/退回路径可吃
//   (2) t_tidy_sm：Soft/Hard/Periodic 旋钮与 maybe_tidy / tidy_snap / polish 烟测
//   (3) t_frag：最大洞 < part_cap(part_lo)、合计空闲够 ⇒ choose_part 降到 frag_floor 区间
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#define private public
#include "fcs.h"
#undef private
using namespace fcs;

namespace {

int chk(bool ok, const char* msg, int& bad) {
    if (!ok) { std::printf("  FAIL: %s\n", msg); ++bad; }
    return ok ? 0 : 1;
}

int t_protect() {
    int bad = 0;
    RamDev dev(8192);
    u8* arena = new u8[512 * PAGE];
    Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_protect open fail\n"); delete[] arena; return 1; }
    s.set_protect_merge_holes(true);
    s.set_hole_protect_pages(64);

    u32 a = 0, b = 0, c = 0;
    chk(s.alloc_pages(128, a, true) == RC::OK, "maint alloc 128", bad);
    chk(s.alloc_pages(64, b, true) == RC::OK, "maint alloc 64", bad);
    chk(a + 128 == b, "contiguous tail growth", bad);
    s.free_[a] = 128;
    s.coalesce();

    const u64 sk0 = s.hole_protect_skips();
    chk(s.alloc_pages(32, c, false) == RC::OK, "user alloc 32", bad);
    chk(c != a, "user must not eat mid hole", bad);
    chk(s.hole_protect_skips() > sk0, "skip counter bumped", bad);

    bool hole_alive = false;
    for (auto& kv : s.free_) if (kv.first <= a && kv.first + kv.second > a) hole_alive = true;
    chk(hole_alive, "mid hole still free", bad);

    {
        s.stream_alloc_ = true;
        chk(s.alloc_pages(32, c, false) == RC::OK, "stream alloc 32", bad);
        s.stream_alloc_ = false;
    }
    chk(c == a, "stream may eat mid hole", bad);

    // 裸 alloc 未建 run ⇒ 不做 verify（所有权探针会报无主页）；只断言分配策略
    u32 d = 0;
    chk(s.alloc_pages(16, d, false) == RC::OK, "user alloc after partial eat", bad);
    chk(d != a, "user still skips remainder of hole region", bad);

    std::printf("  t_protect: skips=%llu a=%u stream_c=%u user_d=%u  %s\n",
                (unsigned long long)s.hole_protect_skips(), a, c, d, bad ? "FAIL" : "ok");
    delete[] arena;
    return bad;
}

int t_frag() {
    int bad = 0;
    RamDev dev(4096);
    u8* arena = new u8[256 * PAGE];
    Store s;
    if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_frag open fail\n"); delete[] arena; return 1; }
    s.set_part_frag(true);
    s.set_part_frag_floor(16);
    // 合成碎片：最大洞 80 页（装不下 part_lo=64 的 cap≈104），但两洞合计 160 ≥ 104
    s.tail_ = 400;
    s.free_.clear();
    s.free_[10] = 80;
    s.free_[200] = 80;
    Store::MergeJob j;
    j.part_lo = 64;
    j.part_hi = 1024;
    const u64 f0 = s.part_frags();
    s.choose_part(j);
    chk(s.part_frags() == f0 + 1, "frag path taken", bad);
    chk(j.part > 0 && j.part < 64, "part lowered below part_lo", bad);
    chk(Store::part_cap_of(j.part) <= 80, "part fits largest hole", bad);

    // 关碎片 ⇒ 仍按 part_lo 长尾
    s.set_part_frag(false);
    j.part = 0; j.part_lo = 64; j.part_hi = 1024;
    s.choose_part(j);
    chk(j.part == 64, "frag off keeps part_lo", bad);

    // 合计不够 ⇒ 不降
    s.set_part_frag(true);
    s.free_.clear();
    s.free_[10] = 80;   // sum=80 < part_cap(64)=104
    j.part = 0; j.part_lo = 64; j.part_hi = 1024;
    const u64 f1 = s.part_frags();
    s.choose_part(j);
    chk(s.part_frags() == f1, "sum too small ⇒ no frag", bad);
    chk(j.part == 64, "no frag keeps part_lo", bad);

    // 再跑一次拿降片结果供日志
    s.free_.clear(); s.free_[10] = 80; s.free_[200] = 80;
    j.part_lo = 64; j.part_hi = 1024; s.choose_part(j);
    std::printf("  t_frag: lowered part=%u (cap=%llu≤80) frags=%llu  %s\n",
                j.part, (unsigned long long)Store::part_cap_of(j.part), (unsigned long long)s.part_frags(),
                bad ? "FAIL" : "ok");
    delete[] arena;
    return bad;
}

// 有界并洞 B：白盒搭 [窗外洞][F1][小 run][F2]，need = F1+run+F2 ⇒ 搬开小 run 一次
int t_coal() {
    int bad = 0;
    RamDev dev(8192);
    u8* arena = new u8[1024 * PAGE];
    Store s;
    if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_coal open fail\n"); delete[] arena; return 1; }
    // mode 2：搭建期 commit 不跑维护（否则会吃掉刚放出的洞）
    s.set_maint_mode(2); s.set_lvl_t(1000); s.set_merge_budget_pages(64);
    s.set_part_coalesce(true); s.set_part_frag(false);
    std::vector<u8> v(200, 3);
    for (u32 i = 0; i < 10; ++i) { s.put(1000000 + i, v.data(), 200); }
    chk(s.commit() == RC::OK, "seed commit", bad);

    auto fill = [&](u32 n) -> u32 {
        u32 p = 0; chk(s.alloc_pages(n, p, true) == RC::OK, "fill alloc", bad); return p;
    };
    u32 dest = fill(40);                         // 挡路 run 目的地（窗外）
    u32 wall = fill(4);                          // 墙：防止 dest 与 F1 并成一个洞（目的地必须在窗外）
    u32 f1 = fill(50);
    std::vector<u32> before; for (auto& kv : s.runs_) before.push_back(kv.first);
    for (u32 i = 0; i < 300; ++i) s.put(20000 + i, v.data(), 200);
    chk(s.commit() == RC::OK, "blocker commit", bad);
    Run* blocker = nullptr; u32 nnew = 0;
    for (auto& kv : s.runs_) if (std::find(before.begin(), before.end(), kv.first) == before.end()) { blocker = kv.second; ++nnew; }
    chk(nnew == 1 && blocker, "one blocker run", bad);
    u32 f2 = fill(50);
    s.free_pages(dest, 40, true); s.free_pages(f1, 50, true); s.free_pages(f2, 50, true);
    // wall 不释放（一直占着）
    chk(s.commit() == RC::OK, "free→pending commit", bad);
    s.pending_free_.clear();                     // 白盒：绕过 SB 闸，直接进 free_
    s.free_[dest] = 40; s.free_[f1] = 50; s.free_[f2] = 50; s.coalesce();
    (void)wall;

    const u32 bblk = s.run_block(blocker);
    const u64 need = 50ull + bblk + 50;
    chk(s.largest_hole() < need, "largest < need before coal", bad);
    if (s.free_page_count() < need) {
        std::printf("  t_coal diag: free=%llu largest=%llu need=%llu blocker@%u blk=%u\n",
                    (unsigned long long)s.free_page_count(), (unsigned long long)s.largest_hole(),
                    (unsigned long long)need, blocker->page0, bblk);
        for (auto& kv : s.free_) if (kv.first < s.tail_)
            std::printf("    free [%u,+%u)\n", kv.first, kv.second);
    }
    chk(s.free_page_count() >= need, "sum >= need", bad);

    std::vector<Store::ManEnt> delta;
    const u64 c0 = s.part_coalesces();
    const u32 bp0 = blocker->page0;
    const u64 bseq = blocker->seq;
    chk(s.coalesce_hole_step(need, delta), "coalesce_hole_step", bad);
    chk(s.part_coalesces() == c0 + 1, "coalesce counter", bad);
    chk(!delta.empty(), "delta from move", bad);
    if (!delta.empty()) chk(s.commit_delta(delta) == RC::OK, "commit_delta", bad);
    Run* nb = nullptr;
    for (auto& kv : s.runs_) if (kv.second->seq == bseq) nb = kv.second;
    chk(nb && nb->page0 != bp0, "blocker relocated", bad);

    s.set_part_coalesce(false);
    std::vector<Store::ManEnt> d2;
    chk(!s.coalesce_hole_step(need, d2), "coal off ⇒ no move", bad);

    s.free_[wall] = 4; s.coalesce();             // 墙还回去，否则 verify 报无主页
    for (int i = 0; i < 2; ++i) if (s.commit() != RC::OK) bad++;
    Report rep; RC vr = s.verify(&rep);
    if (vr != RC::OK || rep.errors) {
        std::printf("  t_coal verify: rc=%s err=%u first=%s\n", rc_str(vr), rep.errors, rep.first_err);
        bad++;
    }
    std::printf("  t_coal: need=%llu blocker %u→%u coalesces=%llu  %s\n",
                (unsigned long long)need, bp0, nb ? nb->page0 : 0,
                (unsigned long long)s.part_coalesces(), bad ? "FAIL" : "ok");
    delete[] arena;
    return bad;
}

int t_tidy_sm() {
    int bad = 0;
    RamDev dev(16384);
    u8* arena = new u8[1024 * PAGE];
    Store s;
    if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_tidy_sm open fail\n"); delete[] arena; return 1; }

    s.set_tidy_policy(Store::TidyPolicy::Soft);
    chk(s.tidy_policy() == Store::TidyPolicy::Soft, "policy Soft", bad);
    chk(s.protect_merge_holes(), "Soft enables protect", bad);
    chk(s.merge_cheap_pages() == 1024, "Soft enables cheap", bad);
    auto sn = s.tidy_snap();
    chk(sn.policy == (int)Store::TidyPolicy::Soft, "snap policy", bad);
    chk(s.maybe_tidy() == RC::OK, "Soft maybe_tidy no-op", bad);

    // 造一点数据，Hard 在低阈值下应能迈出 consolidate_step
    std::vector<u8> v(32, 7);
    for (u32 i = 0; i < 8000; ++i) {
        s.put(i + 1, v.data(), 32);
        if ((i + 1) % 200 == 0) s.commit();
    }
    s.commit();
    s.set_tidy_runs(1, 1);   // 极低阈值 ⇒ 有压力
    s.set_tidy_policy(Store::TidyPolicy::Hard);
    RC r = s.maybe_tidy();
    chk(r == RC::OK || r == RC::MORE, "Hard maybe_tidy step", bad);

    s.set_tidy_policy(Store::TidyPolicy::Periodic);
    r = s.maybe_tidy();
    chk(r == RC::OK || r == RC::MORE, "Periodic maybe_tidy step", bad);

    chk(s.polish() == RC::OK, "polish", bad);
    sn = s.tidy_snap();
    chk(sn.runs >= 1, "after polish still has runs", bad);
    Report rep; chk(s.verify(&rep) == RC::OK, "verify after tidy", bad);

    s.set_tidy_policy(Store::TidyPolicy::Off);
    chk(s.tidy_policy() == Store::TidyPolicy::Off, "Off", bad);
    chk(s.maybe_tidy() == RC::OK, "Off no-op", bad);

    std::printf("  t_tidy_sm: runs=%u layout=%u%% protect=%d  %s\n",
                sn.runs, sn.layout_pct, (int)sn.protect_holes, bad ? "FAIL" : "ok");
    delete[] arena;
    return bad;
}

} // namespace

int main() {
    int bad = 0;
    bad += t_protect();
    bad += t_frag();
    bad += t_coal();
    bad += t_tidy_sm();
    std::printf("g18 护洞+整理态+碎片A+并洞B: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
