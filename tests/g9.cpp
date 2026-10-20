// g9 —— 墓碑账目口径回归（第 11 轮：坑 92 / 93 / 94 / 95）+ 风暴探针固定配置
//
//   坑 93：add_tomb 同 lo 支配规则删旧墓碑时不看存活快照 ⇒ 快照视图里被删键复活（rollback 不受影响）
//   坑 92：单元压实把某键整个丢掉后，老 run 里的点墓碑成了悬空记录；exact_live_recs 整条扫描把它算活 ⇒
//          全死 run 被"校准"成 live=1 永不退休 + verify `run live recs`
//   坑 95：settle_ranges 整盖快路径把点墓碑清零、归并口径算活 ⇒ 快照钉住 run 时 verify `live mismatch`
//   风暴：bench/probe/storm.cpp 的固定配置（每次提交逐键比对当前视图 + 每个存活快照视图，末尾 verify）
//
// 每段都有界：固定提交数、固定键数；整段墙钟约 5 s（2 核沙箱）。
#define STORM_NO_MAIN
#include "../bench/probe/storm.cpp"
#include <chrono>

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void putk(Store& s, u64 k, u32 tag, u32 len = 100) {
    std::vector<u8> v(len, (u8)tag); std::memcpy(v.data(), &tag, 4); s.put(k, v.data(), (u16)len);
}

// 坑 93：S0 钉住老 run；A = del_range(100,105)；S1；B = del_range(100,110) 支配 A。
// S1 视图下 100 必须一直 NOTFOUND；rollback(S1) 之后当前视图也必须 NOTFOUND。
int t93() {
    static u8 arena[128 * PAGE];
    RamDev dev(4096);
    Store s; if (s.open(&dev, arena, 128) != RC::OK) { std::printf("  t93 open fail\n"); return 1; }
    u32 v = 0xABCD; s.put(100, &v, 4); s.put(101, &v, 4); s.commit();
    for (u64 k = 1000; k < 1400; ++k) s.put(k, &v, 4);
    s.commit();
    s.snapshot();                                        // S0
    s.del_range(100, 105); s.commit();                   // A
    s.snapshot(); u32 view = s.last_commit();           // S1
    u8 buf[16]; u16 len = 0;
    RC r1 = s.get_at(100, view, buf, 16, &len);
    u32 t1 = s.range_tombstones();
    s.del_range(100, 110); s.commit();                   // B：同 lo、更宽、更新
    RC r2 = s.get_at(100, view, buf, 16, &len);
    RC r3 = s.get(100, buf, 16, &len);
    u32 t2 = s.range_tombstones();
    RC rb = s.rollback(1);
    RC r4 = s.get(100, buf, 16, &len);
    s.forget_snapshots();
    u32 t3 = s.range_tombstones();
    int bad = 0;
    if (r1 != RC::NOTFOUND || r2 != RC::NOTFOUND) { std::printf("  t93 FAIL: S1 view get_at(100) before/after B = %d/%d (want NOTFOUND)\n", (int)r1, (int)r2); bad++; }
    if (r3 != RC::NOTFOUND) { std::printf("  t93 FAIL: current get(100)=%d\n", (int)r3); bad++; }
    if (t2 != t1 + 1) { std::printf("  t93 FAIL: tombs %u -> %u (want +1: 快照期 A 必须保留)\n", t1, t2); bad++; }
    if (rb != RC::OK || r4 != RC::NOTFOUND) { std::printf("  t93 FAIL: rollback rc=%d get(100)=%d\n", (int)rb, (int)r4); bad++; }
    std::printf("  t93 快照期同 lo 墓碑并存: S1 视图 %s/%s, tombs %u->%u, rollback 后 %s, 释放快照后剪枝 tombs=%u  %s\n",
                r1 == RC::NOTFOUND ? "NOTFOUND" : "FOUND", r2 == RC::NOTFOUND ? "NOTFOUND" : "FOUND", t1, t2,
                r4 == RC::NOTFOUND ? "NOTFOUND" : "FOUND", t3, bad ? "FAIL" : "ok");
    return bad;
}

// 坑 92：R = 含点墓碑 T(1000) 的归一 run；put(1000) 新值；del_range 盖住 1000 ⇒ 压实把 1000 整个丢掉、R 在该单元无段；
// 覆盖 R 其余记录 ⇒ R 必须退休，verify 必须 OK（以前：live_recs=1 幻影，verify `run live recs`）。
int t92() {
    const u64 N = 2000;
    static u8 arena[256 * PAGE];
    RamDev dev(8192);
    Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t92 open fail\n"); return 1; }
    for (u64 k = 1; k <= N; ++k) putk(s, k, 1);
    s.commit();
    s.del(1000);
    for (u64 k = 1; k <= N; ++k) if (k != 1000) putk(s, k, 2);
    s.commit();
    s.consolidate();
    putk(s, 1000, 3); s.commit();
    s.del_range(800, 1300); s.commit();
    for (int i = 0; i < 3; ++i) { putk(s, 2001 + i, 9, 2000); s.commit(); }
    for (u64 k = 1; k <= N; ++k) if (k < 800 || k >= 1300) putk(s, k, 4);
    s.commit();
    for (int i = 0; i < 3; ++i) { putk(s, 2001 + i, 10, 2000); s.commit(); }
    Report rep; RC rc = s.verify(&rep);
    int bad = 0;
    if (rc != RC::OK || rep.errors) { std::printf("  t92 FAIL: verify rc=%d errors=%u first=%s key=%llu\n", (int)rc, rep.errors, rep.first_err, (unsigned long long)rep.first_key); bad++; }
    if (rep.recs > 3000) { std::printf("  t92 FAIL: recs=%u（全死的归一 run 没退休）\n", rep.recs); bad++; }
    std::printf("  t92 悬空点墓碑不再钉住全死 run: runs=%u recs=%u verify=%s  %s\n", s.run_count(), rep.recs, rc == RC::OK && !rep.errors ? "OK" : "FAIL", bad ? "FAIL" : "ok");
    return bad;
}

// 坑 95：点墓碑 run + 快照钉住 + 整库整盖 del_range ⇒ 快路径与归并口径必须一致（verify OK），读全部 NOTFOUND。
int t95() {
    static u8 arena[256 * PAGE];
    RamDev dev(8192);
    Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t95 open fail\n"); return 1; }
    u8 v[100] = {1};
    for (u64 k = 1; k <= 2000; ++k) s.put(k, v, 100);
    s.commit();
    for (u64 k = 900; k <= 1100; k += 7) s.del(k);
    s.commit();
    s.snapshot();
    s.del_range(1, 3000); s.commit();
    Report rep; RC rc = s.verify(&rep);
    u8 buf[128]; u16 len;
    int bad = 0;
    if (rc != RC::OK || rep.errors) { std::printf("  t95 FAIL: verify rc=%d errors=%u first=%s key=%llu\n", (int)rc, rep.errors, rep.first_err, (unsigned long long)rep.first_key); bad++; }
    for (u64 k : {1ull, 500ull, 1000ull, 2000ull}) if (s.get(k, buf, 128, &len) != RC::NOTFOUND) { std::printf("  t95 FAIL: get(%llu) != NOTFOUND\n", (unsigned long long)k); bad++; }
    s.forget_snapshots();
    std::printf("  t95 整盖快路径 vs 归并口径（点墓碑）: verify=%s runs=%u  %s\n", rc == RC::OK && !rep.errors ? "OK" : "FAIL", s.run_count(), bad ? "FAIL" : "ok");
    return bad;
}

// 坑 94：点墓碑在"下面没有更老版本"时必须随归并/压实消失；在"下面还有更老版本"或有快照钉住时必须保留。
//   (a) 队列负载（写一次/删一次 + 全库归一）：5 轮后 recs 必须为 0（老引擎 5000 → 25000 线性涨）；
//   (b) 快照钉住老值：墓碑不能丢（快照视图仍读到老值、当前视图 NOTFOUND）；释放快照并归一后 recs 归零；
//   (c) 底下还有老 run：只合并上层小 run（level_pass），被删键必须一直 NOTFOUND（丢错墓碑 = 复活）；
//       最后全库归一，recs == 仍存活的键数。
int t94() {
    int bad = 0;
    u8 v[100] = {1}; u8 buf[128]; u16 len;
    {   // (a)
        static u8 arena[256 * PAGE];
        RamDev dev(8192);
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t94a open fail\n"); return 1; }
        u32 recs_last = 0;
        for (int round = 0; round < 5; ++round) {
            for (u64 k = 1; k <= 3000; ++k) s.put(round * 3000 + k, v, 100);
            s.commit();
            for (u64 k = 1; k <= 3000; ++k) s.del(round * 3000 + k);
            s.commit();
            s.consolidate();
            Report rep; RC rc = s.verify(&rep);
            if (rc != RC::OK || rep.errors) { std::printf("  t94a FAIL: verify rc=%d errors=%u first=%s\n", (int)rc, rep.errors, rep.first_err); bad++; }
            recs_last = rep.recs;
        }
        if (recs_last != 0) { std::printf("  t94a FAIL: 5 轮队列负载归一后 recs=%u（墓碑没被丢）\n", recs_last); bad++; }
        for (u64 k : {1ull, 3000ull, 7777ull, 15000ull}) if (s.get(k, buf, 128, &len) != RC::NOTFOUND) { std::printf("  t94a FAIL: get(%llu) != NOTFOUND\n", (unsigned long long)k); bad++; }
        std::printf("  t94a 队列负载归一后墓碑消失: recs=%u runs=%u  %s\n", recs_last, s.run_count(), bad ? "FAIL" : "ok");
    }
    {   // (b)
        int b0 = bad;
        static u8 arena[256 * PAGE];
        RamDev dev(8192);
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t94b open fail\n"); return bad + 1; }
        for (u64 k = 1; k <= 2000; ++k) putk(s, k, 7);
        s.commit();
        s.snapshot(); u32 sv = s.last_commit();
        for (u64 k = 1; k <= 2000; ++k) s.del(k);
        s.commit();
        s.consolidate();
        Report rep; RC rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  t94b FAIL: verify rc=%d errors=%u first=%s\n", (int)rc, rep.errors, rep.first_err); bad++; }
        for (u64 k : {1ull, 999ull, 2000ull}) {
            if (s.get(k, buf, 128, &len) != RC::NOTFOUND) { std::printf("  t94b FAIL: 当前视图 get(%llu) != NOTFOUND\n", (unsigned long long)k); bad++; }
            if (s.get_at(k, sv, buf, 128, &len) != RC::OK) { std::printf("  t94b FAIL: 快照视图 get(%llu) 丢了老值\n", (unsigned long long)k); bad++; }
        }
        s.forget_snapshots();
        s.consolidate();
        rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  t94b FAIL: 释放快照后 verify rc=%d errors=%u first=%s\n", (int)rc, rep.errors, rep.first_err); bad++; }
        if (rep.recs != 0) { std::printf("  t94b FAIL: 释放快照并归一后 recs=%u\n", rep.recs); bad++; }
        std::printf("  t94b 快照钉住时墓碑保留、释放后消失: recs=%u  %s\n", rep.recs, bad > b0 ? "FAIL" : "ok");
    }
    {   // (c)
        int b0 = bad;
        static u8 arena[256 * PAGE];
        RamDev dev(8192);
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t94c open fail\n"); return bad + 1; }
        for (u64 k = 1; k <= 2000; ++k) putk(s, k, 3);
        s.commit();
        s.consolidate();                      // 老 run A：2000 键
        // 40 次小提交：每次删 10 个键 + 改写 10 个键；level_pass 会反复合并这些小 run，而 A 一直在底下
        u64 dk = 1;
        for (int i = 0; i < 40; ++i) {
            for (int j = 0; j < 10; ++j) s.del(dk++);
            for (int j = 0; j < 10; ++j) putk(s, 1500 + (u64)(i * 10 + j) % 400, 100 + i);
            s.commit();
            for (u64 k = 1; k < dk; k += 37) if (s.get(k, buf, 128, &len) != RC::NOTFOUND) { std::printf("  t94c FAIL: 提交 %d 后 get(%llu) 复活\n", i, (unsigned long long)k); bad++; break; }
        }
        Report rep; RC rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  t94c FAIL: verify rc=%d errors=%u first=%s\n", (int)rc, rep.errors, rep.first_err); bad++; }
        for (u64 k = 1; k < dk; ++k) if (s.get(k, buf, 128, &len) != RC::NOTFOUND) { std::printf("  t94c FAIL: get(%llu) 复活\n", (unsigned long long)k); bad++; break; }
        s.consolidate();
        rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  t94c FAIL: 归一后 verify rc=%d errors=%u first=%s\n", (int)rc, rep.errors, rep.first_err); bad++; }
        const u32 expect = 2000 - (u32)(dk - 1);
        if (rep.recs != expect) { std::printf("  t94c FAIL: 归一后 recs=%u 期望 %u（墓碑残留或丢键）\n", rep.recs, expect); bad++; }
        for (u64 k = dk; k <= 2000; k += 97) if (s.get(k, buf, 128, &len) != RC::OK) { std::printf("  t94c FAIL: 存活键 get(%llu) 丢失\n", (unsigned long long)k); bad++; break; }
        std::printf("  t94c 底下有老 run 时墓碑保留、归一后精确: recs=%u 期望 %u  %s\n", rep.recs, expect, bad > b0 ? "FAIL" : "ok");
    }
    return bad;
}

} // namespace

int main() {
    double t0 = now_s();
    int bad = 0;
    bad += t93();
    bad += t92();
    bad += t95();
    bad += t94();
    // 风暴固定配置（flags 含义见 bench/probe/storm.cpp 文件头）；RamDev 8192 页足够这些规模
    struct Cfg { const char* flags; int seed; u32 nkeys, commits, per; };
    const Cfg cfgs[] = { {"r", 1, 3000, 120, 200}, {"rsb", 1, 3000, 120, 200}, {"rsb", 2, 2000, 100, 150},
                         {"rd", 2, 3000, 120, 200}, {"rsboalc", 4, 3000, 120, 200}, {"rsc", 1, 2000, 100, 150} };
    for (const Cfg& c : cfgs) bad += storm_run(c.flags, c.seed, c.nkeys, c.commits, c.per, 8192);
    std::printf("g9 墓碑账目口径（坑 92/93/94/95）+ 风暴 %zu 组: %s (%.1f s)\n", sizeof(cfgs) / sizeof(cfgs[0]), bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
