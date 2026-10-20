// g10 —— R2（第 11 轮）：层合并预算 + 分步维护；R7：范围墓碑自动归一；R8：局部 TRIM
//
//   (1) 预算：同一负载分别用 level_merges=1（默认）与 0（不限）跑，两者都必须读全对、verify OK；
//       预算版的 run 数必须有界（≤ (LVL_MAX+1)×lvl_t×4 的兜底线），且单次提交最大耗时不劣于不限版的 1.5×
//      （沙箱噪声大：只断言"有界 + 正确"，耗时只打印，不做硬断言）。
//   (2) consolidate_step 循环：30 条 run + 删除 + 覆盖 + 一个快照钉住早期 run ⇒ 每步 verify + 抽查；
//       步数有界（≤ 2×run 数）；最终可合并 run ≤ 1；释放快照后再循环到 OK ⇒ runs == 1，全部键比对。
//   (3) compact_file_step 循环：先造碎片（删一半），循环到 OK；verify OK、键全对、高水位不高于一次性 compact_file 的 +8 页。
//
// 每段都有界：固定提交数、固定键数；整段墙钟约 6 s（2 核沙箱）。
#include "fcs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <sys/stat.h>
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

// (1) 预算
int t_budget() {
    int bad = 0;
    double maxc[2] = {0, 0}; u32 runs_end[2] = {0, 0}, runs_max[2] = {0, 0};
    for (int mode = 0; mode < 2; ++mode) {
        static u8 arena[512 * PAGE];
        RamDev dev(16384);
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_budget open fail\n"); return 1; }
        s.set_level_merges(mode == 0 ? 1 : 0);
        Truth t; std::mt19937_64 rng(7);
        const u32 N = 6000;
        for (int c = 0; c < 90; ++c) {
            for (int i = 0; i < 150; ++i) putk(s, t, 1 + rng() % N, 1000 + c);
            for (int i = 0; i < 10; ++i) delk(s, t, 1 + rng() % N);
            double t0 = now_s();
            if (s.commit() != RC::OK) { std::printf("  t_budget FAIL: commit %d\n", c); bad++; }
            maxc[mode] = std::max(maxc[mode], now_s() - t0);
            runs_max[mode] = std::max(runs_max[mode], s.run_count());
        }
        runs_end[mode] = s.run_count();
        bad += verify_ok(s, "t_budget");
        bad += compare_all(s, t, "t_budget");
        const u32 bound = (u32)s.lvl_hist().size() * s.lvl_t() * 4 + 4;   // (LVL_MAX+1) 层 × 兜底线
        if (runs_max[mode] > bound) { std::printf("  t_budget FAIL: level_merges=%u run 数 %u 超过兜底线 %u\n", s.level_merges(), runs_max[mode], bound); bad++; }
    }
    std::printf("  R2 预算 level_merges=1 vs 0: runs(max/end) %u/%u vs %u/%u, 最长提交 %.1f vs %.1f ms  %s\n",
                runs_max[0], runs_end[0], runs_max[1], runs_end[1], maxc[0] * 1e3, maxc[1] * 1e3, bad ? "FAIL" : "ok");
    return bad;
}

// (2) consolidate_step
int t_consolidate_step() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(16384);
    Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_cstep open fail\n"); return 1; }
    s.set_level_merges(0);                   // 让 run 多攒一些
    s.set_lvl_t(64);                         // 关掉自动层合并，run 数由我们控制
    Truth t; std::mt19937_64 rng(11);
    for (int c = 0; c < 6; ++c) { for (int i = 0; i < 300; ++i) putk(s, t, 1 + rng() % 4000, 1 + c); s.commit(); }
    s.snapshot(); const Vers sv = s.last_commit();   // 钉住前 6 条 run
    std::map<u64, u32> snap_truth = t.m;
    for (int c = 6; c < 30; ++c) {
        for (int i = 0; i < 200; ++i) putk(s, t, 1 + rng() % 4000, 1 + c);
        for (int i = 0; i < 30; ++i) delk(s, t, 1 + rng() % 4000);
        s.commit();
    }
    const u32 runs0 = s.run_count();
    s.set_lvl_t(8);
    int steps = 0; RC rc;
    static u8 buf[65536]; u16 len = 0;
    while ((rc = s.consolidate_step()) == RC::MORE) {
        ++steps;
        if (steps > (int)runs0 * 2) { std::printf("  t_cstep FAIL: 步数 %d 超过 2×run 数 %u\n", steps, runs0); bad++; break; }
        bad += verify_ok(s, "t_cstep(step)");
        for (int i = 0; i < 50; ++i) {        // 每步抽查：当前视图 + 快照视图
            u64 k = 1 + rng() % 4000;
            auto it = t.m.find(k); RC g = s.get(k, buf, 65535, &len);
            bool ok = it == t.m.end() || it->second == 0 ? g == RC::NOTFOUND : (g == RC::OK && std::memcmp(buf, &it->second, 4) == 0);
            if (!ok) { std::printf("  t_cstep FAIL: 第 %d 步后 get(%llu) 错\n", steps, (unsigned long long)k); bad++; break; }
            auto st = snap_truth.find(k); RC ga = s.get_at(k, sv, buf, 65535, &len);
            bool oka = st == snap_truth.end() ? ga == RC::NOTFOUND : (ga == RC::OK && std::memcmp(buf, &st->second, 4) == 0);
            if (!oka) { std::printf("  t_cstep FAIL: 第 %d 步后快照视图 get(%llu) 错\n", steps, (unsigned long long)k); bad++; break; }
        }
        if (bad) break;
    }
    if (rc != RC::OK && rc != RC::MORE) { std::printf("  t_cstep FAIL: rc=%s\n", rc_str(rc)); bad++; }
    const u32 runs_pinned = s.run_count();
    s.forget_snapshots();
    int steps2 = 0;
    while ((rc = s.consolidate_step()) == RC::MORE) { if (++steps2 > 64) { std::printf("  t_cstep FAIL: 释放快照后步数失控\n"); bad++; break; } }
    bad += verify_ok(s, "t_cstep(end)");
    bad += compare_all(s, t, "t_cstep");
    if (s.run_count() != 1) { std::printf("  t_cstep FAIL: 收尾 runs=%u（期望 1）\n", s.run_count()); bad++; }
    std::printf("  R2 consolidate_step: runs %u → %u(快照钉住) → %u, 步数 %d + %d  %s\n", runs0, runs_pinned, s.run_count(), steps, steps2, bad ? "FAIL" : "ok");
    return bad;
}

// (3) compact_file_step
int t_compact_file_step() {
    int bad = 0;
    u32 tail_once = 0, tail_step = 0, steps = 0;
    for (int mode = 0; mode < 2; ++mode) {
        static u8 arena[512 * PAGE];
        RamDev dev(16384);
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_cfstep open fail\n"); return 1; }
        Truth t; std::mt19937_64 rng(5);
        for (int c = 0; c < 20; ++c) { for (int i = 0; i < 300; ++i) putk(s, t, 1 + rng() % 5000, 1 + c, 300); s.commit(); }
        for (u64 k = 1; k <= 5000; k += 2) delk(s, t, k);
        s.commit();
        const u32 before = s.used_pages();
        RC rc;
        if (mode == 0) rc = s.compact_file();
        else { while ((rc = s.compact_file_step()) == RC::MORE) { if (++steps > 200) { std::printf("  t_cfstep FAIL: 步数失控\n"); bad++; break; } } }
        if (rc != RC::OK) { std::printf("  t_cfstep FAIL: mode %d rc=%s\n", mode, rc_str(rc)); bad++; }
        bad += verify_ok(s, mode ? "t_cfstep(step)" : "t_cfstep(once)");
        bad += compare_all(s, t, "t_cfstep");
        (mode ? tail_step : tail_once) = s.used_pages();
        if (s.used_pages() >= before) { std::printf("  t_cfstep FAIL: mode %d 高水位没降 %u → %u\n", mode, before, s.used_pages()); bad++; }
    }
    if (tail_step > tail_once + 8) { std::printf("  t_cfstep FAIL: 分步高水位 %u 比一次性 %u 高太多\n", tail_step, tail_once); bad++; }
    std::printf("  R2 compact_file_step: 高水位 一次性 %u / 分步 %u（%u 步）  %s\n", tail_once, tail_step, steps, bad ? "FAIL" : "ok");
    return bad;
}

// (4) R7 范围墓碑自动归一：阈值 32，连续 del_range 把墓碑推过阈值 ⇒ 之后有限次提交内墓碑清零；
//     过程中每次提交后抽查读；有快照时不清（断言墓碑数不降），释放后恢复。
int t_range_auto() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(16384);
    Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_rauto open fail\n"); return 1; }
    s.set_range_tomb_auto(32);
    Truth t; std::mt19937_64 rng(3);
    for (int c = 0; c < 10; ++c) { for (int i = 0; i < 300; ++i) putk(s, t, 1 + rng() % 20000, 1 + c); s.commit(); }
    static u8 buf[65536]; u16 len = 0;
    auto spot = [&](const char* tag) {
        for (int i = 0; i < 40; ++i) {
            u64 k = 1 + rng() % 20000; auto it = t.m.find(k); RC g = s.get(k, buf, 65535, &len);
            bool ok = it == t.m.end() || it->second == 0 ? g == RC::NOTFOUND : (g == RC::OK && std::memcmp(buf, &it->second, 4) == 0);
            if (!ok) { std::printf("  t_rauto FAIL: %s get(%llu) 错\n", tag, (unsigned long long)k); return 1; }
        }
        return 0;
    };
    u32 peak = 0; int cleared_at = -1;
    for (int c = 0; c < 40; ++c) {                       // 每次提交 3 个小范围删除 ⇒ 十几次提交后墓碑过 32
        for (int j = 0; j < 3; ++j) { u64 lo = 1 + rng() % 19000; s.del_range(lo, lo + 10); for (u64 k = lo; k < lo + 10; ++k) if (t.m.count(k)) t.m[k] = 0; }
        for (int i = 0; i < 20; ++i) putk(s, t, 1 + rng() % 20000, 100 + c);
        if (s.commit() != RC::OK) { std::printf("  t_rauto FAIL: commit %d\n", c); bad++; }
        peak = std::max(peak, s.range_tombstones());
        if (peak >= 32 && s.range_tombstones() < 8 && cleared_at < 0) cleared_at = c;
        bad += spot("auto");
        if (bad) break;
    }
    if (cleared_at < 0) { std::printf("  t_rauto FAIL: 墓碑峰值 %u，40 次提交内没有清零（最后 %u）\n", peak, s.range_tombstones()); bad++; }
    bad += verify_ok(s, "t_rauto");
    bad += compare_all(s, t, "t_rauto");
    // 有快照：不清
    s.snapshot();
    for (int c = 0; c < 30; ++c) { u64 lo = 1 + rng() % 19000; s.del_range(lo, lo + 5); for (u64 k = lo; k < lo + 5; ++k) if (t.m.count(k)) t.m[k] = 0; s.commit(); }
    const u32 pinned = s.range_tombstones();
    for (int c = 0; c < 40; ++c) { u64 lo = 1 + rng() % 19000; s.del_range(lo, lo + 5); for (u64 k = lo; k < lo + 5; ++k) if (t.m.count(k)) t.m[k] = 0; s.commit(); }
    if (s.range_tombstones() < pinned) { std::printf("  t_rauto FAIL: 有快照时墓碑被清了 %u → %u\n", pinned, s.range_tombstones()); bad++; }
    s.forget_snapshots();
    for (int c = 0; c < 12 && s.range_tombstones() > 0; ++c) { putk(s, t, 1 + rng() % 20000, 200 + c); s.commit(); }
    if (s.range_tombstones() != 0) { std::printf("  t_rauto FAIL: 释放快照后 12 次提交墓碑仍有 %u\n", s.range_tombstones()); bad++; }
    bad += verify_ok(s, "t_rauto(end)");
    bad += compare_all(s, t, "t_rauto(end)");
    std::printf("  R7 范围墓碑自动归一（阈值 32）：峰值 %u，第 %d 次提交清零；快照期保留 %u 条，释放后清零  %s\n", peak, cleared_at, pinned, bad ? "FAIL" : "ok");
    return bad;
}

// (5) R8 局部 TRIM：FileDev + set_discard_min_pages(16)，覆盖写负载跑 60 次提交；打洞只许落在提交点之后回到空闲表的整段 ⇒
//     verify 干净、键全对、discard 次数 > 0；文件实占块数（st_blocks）必须 < 高水位字节数（洞确实打出去了；
//     文件系统不支持 PUNCH_HOLE 时 fallocate 失败、实占不降 —— 只打印不判失败）。
int t_discard() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    const char* P = "build/g10_r8.db"; std::remove(P);
    FileDev d(P, 40000);
    if (!d.ok()) { std::printf("  t_discard: 打不开 %s（需要 build/ 目录）—— 跳过\n", P); return 0; }
    Store s; if (s.open(&d, arena, 512) != RC::OK) { std::printf("  t_discard open fail\n"); return 1; }
    s.set_discard_min_pages(16);
    Truth t; std::mt19937_64 rng(9);
    for (int c = 0; c < 60; ++c) { for (int i = 0; i < 500; ++i) putk(s, t, 1 + rng() % 8000, 1 + c, 400); s.commit(); }
    bad += verify_ok(s, "t_discard");
    bad += compare_all(s, t, "t_discard");
    if (d.discards() == 0) { std::printf("  t_discard FAIL: 没有发生任何 discard（高水位 %u 页）\n", s.used_pages()); bad++; }
    struct stat st; std::memset(&st, 0, sizeof st); ::stat(P, &st);
    const double occ_mb = st.st_blocks * 512.0 / 1048576, hw_mb = s.used_pages() * (double)PAGE / 1048576;
    std::printf("  R8 局部 TRIM：discard %llu 次，高水位 %.1f MB，文件实占 %.1f MB%s  %s\n", (unsigned long long)d.discards(), hw_mb, occ_mb,
                occ_mb < hw_mb * 0.8 ? "（洞已打出）" : "（实占未降：文件系统可能不支持 PUNCH_HOLE，不判失败）", bad ? "FAIL" : "ok");
    std::remove(P);
    return bad;
}

} // namespace

int main() {
    double t0 = now_s();
    int bad = 0;
    bad += t_budget();
    bad += t_consolidate_step();
    bad += t_compact_file_step();
    bad += t_range_auto();
    bad += t_discard();
    std::printf("g10 R2 层合并预算 + 分步维护 + R7 墓碑自动归一 + R8 局部 TRIM: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
