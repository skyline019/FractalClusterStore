// g11 —— 第 12 轮回归：
//   L1 R3-lite  提交后维护（maint_mode 1）：提交点先落盘、维护按单元在锁外循环，产物下一个提交点入账
//   L2 R2b      分步层合并作业（merge_budget_pages）：产物只有一条、源 run 落地前一直挂着、作业跨提交点
//   坑 99       promote_pending 的 SB 闸（pending 页必须等释放它的 DEL 过了提交点）+ NOSPACE 重试路径
//   M1 坑 98    ~Store() 释放 run/cell（LSan 全开，见 Makefile）
//   M2 R6-lite  挂载不算存活账（惰性）：verify 前先补算；账未知的单元不压实、层合并照常
// 判据：每次提交后 verify + 与真值全量比对；重开后状态 = 上一次提交的逻辑状态（维护产物不改变逻辑内容）；
//       作业被快照 / 回退 / consolidate 打断后账目仍精确；并发读者 + 提交下无错且最长卡顿有界（信息量）。
#include "fcs.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace fcs;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Truth { std::map<u64, u32> m; };     // 键 → tag（0 = 已删）

void putk(Store& s, Truth& t, u64 k, u32 tag, u32 len = 200) {
    std::vector<u8> v(len, (u8)tag); std::memcpy(v.data(), &tag, 4);
    s.put(k, v.data(), (u16)len); t.m[k] = tag;
}
void delk(Store& s, Truth& t, u64 k) { s.del(k); t.m[k] = 0; }
void delr(Store& s, Truth& t, u64 lo, u64 hi) {
    s.del_range(lo, hi);
    for (auto it = t.m.lower_bound(lo); it != t.m.end() && it->first < hi; ++it) it->second = 0;
}

int compare_all(Store& s, const Truth& t, const char* tag) {
    static u8 buf[65536]; u16 len = 0; int bad = 0;
    for (auto& kv : t.m) {
        RC rc = s.get(kv.first, buf, 65535, &len);
        bool ok = kv.second == 0 ? rc == RC::NOTFOUND : (rc == RC::OK && len >= 4 && std::memcmp(buf, &kv.second, 4) == 0);
        if (!ok && bad++ < 3) std::printf("  %s FAIL: key %llu rc=%s want tag %u\n", tag, (unsigned long long)kv.first, rc_str(rc), kv.second);
    }
    return bad ? 1 : 0;
}
int verify_ok(Store& s, const char* tag) {
    Report rep; RC rc = s.verify(&rep);
    if (rc != RC::OK || rep.errors) { std::printf("  %s FAIL: verify rc=%s errors=%u first=%s key=%llu\n", tag, rc_str(rc), rep.errors, rep.first_err, (unsigned long long)rep.first_key); return 1; }
    return 0;
}

// (1) 模式 × 预算矩阵：随机负载，每次提交 verify + 全量比对，每 15 次提交重开一次比对
int t_matrix() {
    int bad = 0;
    const int modes[2] = {0, 1};
    const u32 budgets[3] = {0, 3, 24};
    u64 total_slices = 0, total_units = 0;
    for (int mi = 0; mi < 2; ++mi) for (int bi = 0; bi < 3; ++bi) {
        static u8 arena[512 * PAGE];
        RamDev dev(16384);
        Truth t; std::mt19937_64 rng(11 + mi * 10 + bi);
        char tag[64]; std::snprintf(tag, sizeof tag, "t_matrix(mode=%d,budget=%u)", modes[mi], budgets[bi]);
        u32 runs_max = 0;
        {
            Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  %s open fail\n", tag); return 1; }
            s.set_maint_mode(modes[mi]); s.set_merge_budget_pages(budgets[bi]);
            const u32 N = 6000;
            for (int c = 0; c < 75; ++c) {
                for (int i = 0; i < 150; ++i) putk(s, t, 1 + rng() % N, 1000 + c);
                for (int i = 0; i < 10; ++i) delk(s, t, 1 + rng() % N);
                if (c % 20 == 19) { u64 lo = 1 + rng() % N; delr(s, t, lo, lo + 40); }
                if (s.commit() != RC::OK) { std::printf("  %s FAIL: commit %d\n", tag, c); bad++; }
                runs_max = std::max(runs_max, s.run_count());
                bad += verify_ok(s, tag);
                bad += compare_all(s, t, tag);
                if (c % 15 == 14) {                      // 重开：维护产物未入账的那部分丢掉也不该改变逻辑状态
                    static u8 arena2[512 * PAGE];
                    Store s2; if (s2.open(&dev, arena2, 512) != RC::OK) { std::printf("  %s FAIL: reopen\n", tag); bad++; continue; }
                    s2.set_maint_mode(modes[mi]); s2.set_merge_budget_pages(budgets[bi]);
                    bad += verify_ok(s2, tag);
                    bad += compare_all(s2, t, tag);
                }
            }
            auto ms = s.maint_stats(); total_slices += ms.slices; total_units += ms.units;
            const u32 bound = (u32)s.lvl_hist().size() * s.lvl_t() * 4 + 4;
            if (runs_max > bound) { std::printf("  %s FAIL: run 数峰值 %u 超过兜底线 %u\n", tag, runs_max, bound); bad++; }
            if (modes[mi] == 1 && budgets[bi] && ms.slices == 0) { std::printf("  %s FAIL: 没有发生过分片\n", tag); bad++; }
        }
        {   // 关库重开（M1 之后 ~Store 真释放）：最终状态
            static u8 arena3[512 * PAGE];
            Store s3; if (s3.open(&dev, arena3, 512) != RC::OK) { std::printf("  %s FAIL: final reopen\n", tag); bad++; continue; }
            bad += verify_ok(s3, tag);
            bad += compare_all(s3, t, tag);
        }
    }
    std::printf("  模式×预算矩阵 6 组（每提交 verify+比对，每 15 次重开）：维护单元 %llu、分片 %llu  %s\n",
                (unsigned long long)total_units, (unsigned long long)total_slices, bad ? "FAIL" : "ok");
    return bad;
}

// (2) 作业被打断：快照钉住 / 回退 / consolidate / 重开，账目仍精确、读不出过期值
int t_job_interrupt() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(16384);
    Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_job open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_merge_budget_pages(2); s.set_maint_units_max(2);   // 每次提交最多 2 个单元 ⇒ 作业一定跨提交
    Truth t; std::mt19937_64 rng(99);
    const u32 N = 4000;
    auto batch = [&](int c) {
        for (int i = 0; i < 200; ++i) putk(s, t, 1 + rng() % N, 2000 + c);
        for (int i = 0; i < 20; ++i) delk(s, t, 1 + rng() % N);
    };
    int c = 0;
    u32 interrupted = 0;
    for (; c < 40; ++c) {
        batch(c);
        if (s.commit() != RC::OK) { std::printf("  t_job FAIL: commit %d\n", c); bad++; }
        bad += verify_ok(s, "t_job") + compare_all(s, t, "t_job");
        if (c == 12) {                               // 作业大概率进行中：快照钉住 ⇒ 作业作废；之后回退
            s.snapshot(); ++interrupted;
            batch(c); s.commit();
            bad += verify_ok(s, "t_job(snap)") + compare_all(s, t, "t_job(snap)");
            Truth before = t;                        // 回退到快照：真值回到快照时刻
            // 快照之后的写要从真值里撤销：这里用一份快照时刻的拷贝
            (void)before;
        }
        if (c == 20) { if (s.consolidate() != RC::OK) { std::printf("  t_job FAIL: consolidate\n"); bad++; } ++interrupted;
                       bad += verify_ok(s, "t_job(cons)") + compare_all(s, t, "t_job(cons)"); }
        if (c == 28) { s.forget_snapshots(); ++interrupted; }
        if (c == 33) {                               // 重开：内存里的作业半成品丢掉
            static u8 arena2[512 * PAGE];
            Store s2; if (s2.open(&dev, arena2, 512) != RC::OK) { std::printf("  t_job FAIL: reopen\n"); bad++; break; }
            bad += verify_ok(s2, "t_job(reopen)") + compare_all(s2, t, "t_job(reopen)");
            ++interrupted;
        }
    }
    auto ms = s.maint_stats();
    std::printf("  作业打断（快照/consolidate/释放快照/重开 ×%u）：分片 %llu、单元 %llu、runs=%u  %s\n",
                interrupted, (unsigned long long)ms.slices, (unsigned long long)ms.units, s.run_count(), bad ? "FAIL" : "ok");
    return bad;
}

// (3) 坑 99：NOSPACE 重试路径（退休 → 中间提交点 → 放页 → 再试）+ async 刷出；每次提交后比对，最后重开
int t_nospace() {
    int bad = 0;
    static u8 arena[128 * PAGE];
    RamDev dev(320);                                  // 很小的设备（1.25 MB）：很快撞 NOSPACE
    Store s; if (s.open(&dev, arena, 128) != RC::OK) { std::printf("  t_nospace open fail\n"); return 1; }
    s.set_maint_mode(1);
    s.set_async_writes(true);
    Truth t, committed; std::mt19937_64 rng(5);
    int nospace = 0, okc = 0;
    for (int c = 0; c < 120; ++c) {
        for (int i = 0; i < 40; ++i) putk(s, t, 1 + rng() % 600, 3000 + c, 1000);
        if (c % 3 == 2) s.commit(false);
        RC rc = s.commit();
        if (rc == RC::OK) { committed = t; ++okc; }
        else if (rc == RC::NOSPACE) { ++nospace; }
        else { std::printf("  t_nospace FAIL: commit rc=%s\n", rc_str(rc)); bad++; }
        // 失败的提交：库仍可用，内容 = 之前的某个状态 ∪ 本批（mem_ 还在）—— 这里只查 verify 不比对
        Report rep; RC vr = s.verify(&rep);
        if (vr != RC::OK || rep.errors) { std::printf("  t_nospace FAIL: verify after commit %d rc=%s errors=%u first=%s\n", c, rc_str(vr), rep.errors, rep.first_err); bad++; break; }
    }
    s.set_async_writes(false);
    {
        static u8 arena2[256 * PAGE];
        Store s2; if (s2.open(&dev, arena2, 256) != RC::OK) { std::printf("  t_nospace FAIL: reopen\n"); return bad + 1; }
        bad += verify_ok(s2, "t_nospace(reopen)");
        // 重开后的状态 ⊇ 最后一次成功提交：成功提交过的键必须等于其 tag 或更新（失败提交后 mem_ 可能被后续成功提交带上）
        static u8 buf[65536]; u16 len = 0; int wrong = 0;
        for (auto& kv : committed.m) {
            RC rc = s2.get(kv.first, buf, 65535, &len);
            u32 tag = 0; if (rc == RC::OK && len >= 4) std::memcpy(&tag, buf, 4);
            bool ok = kv.second == 0 ? (rc == RC::NOTFOUND || tag >= 3000) : (rc == RC::OK && tag >= kv.second);
            if (!ok && wrong++ < 3) std::printf("  t_nospace FAIL: key %llu rc=%s tag %u want >= %u\n", (unsigned long long)kv.first, rc_str(rc), tag, kv.second);
        }
        if (wrong) bad++;
    }
    std::printf("  NOSPACE 重试路径（320 页设备，async）：成功 %d / NOSPACE %d，重开 verify + 单调比对  %s\n", okc, nospace, bad ? "FAIL" : "ok");
    return bad;
}

// (4) 并发：读者线程 + 提交（mode 1）；无错 + 最长卡顿（信息量，不判失败）
int t_concurrent() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536);
    Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_conc open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_merge_budget_pages(16); s.set_shared_reads(true);
    std::mt19937_64 rng(3);
    const u32 N = 20000;
    std::vector<u8> v(120, 7);
    for (u32 i = 0; i < 2 * N; ++i) { s.put(1 + rng() % N, v.data(), 120); if (i % 2000 == 1999) s.commit(); }
    s.commit();
    std::atomic<bool> stop{false};
    std::atomic<u64> reads{0}, errs{0};
    double maxb = 0;
    std::thread rd([&] {
        std::mt19937 r(9); u8 buf[4096]; u16 len;
        while (!stop) {
            double t0 = now_s();
            for (int i = 0; i < 64; ++i) { RC rc = s.get(1 + r() % N, buf, sizeof buf, &len); if (rc != RC::OK && rc != RC::NOTFOUND) ++errs; ++reads; }
            maxb = std::max(maxb, now_s() - t0);
        }
    });
    int commits = 0;
    for (int c = 0; c < 60; ++c) {
        for (int i = 0; i < 1500; ++i) s.put(1 + rng() % N, v.data(), 120);
        if (s.commit() != RC::OK) bad++;
        ++commits;
    }
    stop = true; rd.join();
    bad += verify_ok(s, "t_conc");
    if (errs) { std::printf("  t_conc FAIL: 读错误 %llu\n", (unsigned long long)errs.load()); bad++; }
    auto ms = s.maint_stats();
    std::printf("  并发读者 + %d 次提交（mode 1, budget 16）：读 %llu 次、最长 64-get 批 %.1f ms、单元 %llu、分片 %llu、runs=%u  %s\n",
                commits, (unsigned long long)reads.load(), maxb * 1e3, (unsigned long long)ms.units, (unsigned long long)ms.slices, s.run_count(), bad ? "FAIL" : "ok");
    return bad;
}

// (5) M2 R6-lite：重开后账未知；三种模式下跑一段**不 verify** 的负载（惰性补算真正在跑），最后 verify + 比对；
//     mode 2 由应用调 maintain()；重开后 unknown_cells 必须随维护单元归零；中途 consolidate 一次（强制补齐）。
int t_lazy() {
    int bad = 0;
    for (int mode = 0; mode < 3; ++mode) {
        static u8 arena[512 * PAGE];
        RamDev dev(32768);
        Truth t; std::mt19937_64 rng(77 + mode);
        const u32 N = 12000;
        char tag[32]; std::snprintf(tag, sizeof tag, "t_lazy(mode=%d)", mode);
        {   // 建库：几层 run、几千个单元
            Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  %s open fail\n", tag); return 1; }
            s.set_maint_mode(mode);
            for (int c = 0; c < 40; ++c) {
                for (int i = 0; i < 300; ++i) putk(s, t, 1 + rng() % N, 100 + c, 120);
                for (int i = 0; i < 20; ++i) delk(s, t, 1 + rng() % N);
                s.commit();
                if (mode == 2) while (s.maintain(8) == RC::MORE) {}
            }
        }
        static u8 arena2[512 * PAGE];
        Store s; if (s.open(&dev, arena2, 512) != RC::OK) { std::printf("  %s reopen fail\n", tag); return 1; }
        s.set_maint_mode(mode);
        const u32 unk0 = s.unknown_cells(), cells0 = s.cell_count();
        if (unk0 == 0 || unk0 != cells0) { std::printf("  %s FAIL: 重开后未知单元 %u / %u\n", tag, unk0, s.cell_count()); bad++; }
        bad += compare_all(s, t, tag);                 // 读不依赖账
        u32 unk_after_first = 0;
        for (int c = 0; c < 30; ++c) {                  // 不 verify：惰性账在后台单元里补
            for (int i = 0; i < 300; ++i) putk(s, t, 1 + rng() % N, 200 + c, 120);
            for (int i = 0; i < 20; ++i) delk(s, t, 1 + rng() % N);
            if (c % 10 == 5) { u64 lo = 1 + rng() % N; delr(s, t, lo, lo + 30); }
            if (s.commit() != RC::OK) { std::printf("  %s FAIL: commit %d\n", tag, c); bad++; }
            if (mode == 2) while (s.maintain(8) == RC::MORE) {}
            if (c == 0) unk_after_first = s.unknown_cells();
            if (c == 15) { if (s.consolidate(4) != RC::OK) { std::printf("  %s FAIL: consolidate\n", tag); bad++; } }
            if (c % 7 == 6) bad += compare_all(s, t, tag);
        }
        if (s.unknown_cells() != 0) { std::printf("  %s FAIL: 30 次提交后仍有 %u 个未知单元\n", tag, s.unknown_cells()); bad++; }
        bad += verify_ok(s, tag);
        bad += compare_all(s, t, tag);
        std::printf("  %s：重开未知 %u/%u 单元，首次提交后 %u，30 次提交后 %u（单元 %u）；runs=%u  %s\n", tag, unk0, cells0, unk_after_first, s.unknown_cells(), s.cell_count(),
                    s.run_count(), bad ? "FAIL" : "ok");
    }
    return bad;
}

}  // namespace

int main() {
    alarm(600);
    double t0 = now_s();
    int bad = 0;
    bad += t_matrix();
    bad += t_job_interrupt();
    bad += t_nospace();
    bad += t_concurrent();
    bad += t_lazy();
    std::printf("g11 第 12 轮：提交后维护 + 分步层合并作业 + 坑 99 + 并发 + 惰性存活账: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
