// g12.cpp —— 坑 102 回归：16 位逻辑提交版本回绕（第 12 轮全量评估时发现）。
// 70000 次逻辑提交（每 256 次一次持久提交），第 65000 次后持有一个快照跨过第 65536 次：
//   老头（u16 Vers）：verify "future ver" 2539 处、快照视图 2000/2000 个键看见未来写、run 8 → 29 条；
//   修后：全程 OK。单跑 ≈ 4 s（RamDev）。与 bench/probe/verswrap.cpp 同源，这里固定参数并给出 PASS/FAIL。
#include "fcs.h"
#include <unistd.h>
#include <cstdio>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
using namespace fcs;
static u8 arena[512 * PAGE];

int main(int argc, char** argv) {
    alarm(120);
    u32 commits = 70000;
    u32 phys    = 256;
    const u32 NK = 2000;
    RamDev d(60000);
    Store s;
    if (s.open(&d, arena, 512) != RC::OK) { puts("open fail"); return 2; }
    std::map<u64, u32> model;
    int bad = 0;
    auto check = [&](const char* tag, u32 n) {
        int miss = 0;
        for (u32 i = 0; i < n; ++i) {
            u64 k = 1 + (u64)((i * 7919u) % NK);
            u32 v = 0; u16 len = 0;
            RC g = s.get(k, &v, sizeof v, &len);
            auto it = model.find(k);
            bool ok = (it == model.end()) ? (g == RC::NOTFOUND) : (g == RC::OK && len == 4 && v == it->second);
            if (!ok && miss++ < 3) std::printf("  [%s] k=%llu get=%s v=%u want=%s%u\n", tag, (unsigned long long)k, rc_str(g), v,
                                                it == model.end() ? "NOTFOUND " : "", it == model.end() ? 0u : it->second);
        }
        if (miss) bad = 1;
        return miss;
    };
    u32 snap_at = 65000; (void)argc; (void)argv;   // >0：在第 snap_at 次逻辑提交后抓一个快照并一直持有（跨回绕）
    std::map<u64, u32> held; u32 held_v = 0; bool held_ok = false;
    u32 i = 0, first_wrap_at = 0; u32 prev = s.last_commit();
    for (; i < commits; ++i) {
        if (snap_at && i == snap_at) {
            RC hr = s.snapshot(); held_v = s.snap_version(0); held = model; held_ok = (hr == RC::OK);
            std::printf("held snapshot at logical commit %u: rc=%s vers=%u\n", i, rc_str(hr), (unsigned)held_v);
        }
        u64 k = 1 + (u64)((i * 48271u) % NK); u32 v = i + 1;
        if (RC r = s.put(k, &v, sizeof v); r != RC::OK) { std::printf("put rc=%s at %u\n", rc_str(r), i); return 2; }
        model[k] = v;
        RC r = (i % phys == phys - 1) ? s.commit(true) : s.commit(false);
        if (r != RC::OK) { std::printf("commit rc=%s at logical commit %u (last_commit=%u)\n", rc_str(r), i, (unsigned)s.last_commit()); return 2; }
        u32 now = s.last_commit();
        if (now < prev && !first_wrap_at) { first_wrap_at = i + 1; std::printf("version wrapped at logical commit %u: %u -> %u\n", i + 1, (unsigned)prev, (unsigned)now); }
        prev = now;
        if ((i + 1) % 10000 == 0) {
            int m = check("mid", 500);
            std::printf("after %u logical commits: last_commit=%u runs=%u miss=%d\n", i + 1, (unsigned)s.last_commit(), s.run_count(), m);
        }
    }
    if (RC r = s.commit(true); r != RC::OK) { std::printf("final commit rc=%s\n", rc_str(r)); return 2; }
    std::printf("done %u logical commits, last_commit=%u, wrapped=%s\n", commits, (unsigned)s.last_commit(), first_wrap_at ? "yes" : "no");
    check("final", NK);
    Report rep; RC vr = s.verify(&rep);
    std::printf("verify rc=%s err=%u first_err=%s\n", rc_str(vr), rep.errors, rep.first_err);
    if (vr != RC::OK || rep.errors) bad = 1;

    if (held_ok) {   // 跨回绕持有的快照：视图里不应出现快照之后的写
        int hm = 0, future = 0;
        for (u32 j = 0; j < NK; ++j) {
            u64 k = 1 + j; u32 v = 0; u16 len = 0;
            RC g = s.get_at(k, held_v, &v, sizeof v, &len);
            auto it = held.find(k);
            bool ok = (it == held.end()) ? (g == RC::NOTFOUND) : (g == RC::OK && v == it->second);
            if (!ok) { ++hm; if (g == RC::OK && v > it->second) ++future; if (hm <= 3) std::printf("  [held-view] k=%llu get_at=%s v=%u want=%u\n", (unsigned long long)k, rc_str(g), v, it == held.end() ? 0u : it->second); }
        }
        std::printf("held snapshot view (vers=%u) across wrap: miss=%d of %u (sees future writes: %d); runs now=%u\n", (unsigned)held_v, hm, NK, future, s.run_count());
        if (hm) bad = 1;
        s.forget_snapshots();
    }
    // 快照 + 覆盖 + 快照视图 + 回退（回绕之后这些还闭合吗）
    RC sr = s.snapshot();
    std::printf("snapshot rc=%s (vers=%u)\n", rc_str(sr), (unsigned)s.snap_version(0));
    if (sr == RC::OK) {
        u32 sv = s.snap_version(0);
        std::map<u64, u32> before = model;
        for (u32 j = 0; j < 3000; ++j) {
            u64 k = 1 + (u64)((j * 48271u) % NK); u32 v = 0xA0000000u + j;
            s.put(k, &v, sizeof v); model[k] = v;
            if (j % 500 == 499) if (RC r = s.commit(true); r != RC::OK) { std::printf("commit after snapshot rc=%s\n", rc_str(r)); return 2; }
        }
        s.commit(true);
        int sm = 0;
        for (u32 j = 0; j < 500; ++j) {
            u64 k = 1 + (u64)((j * 7919u) % NK); u32 v = 0; u16 len = 0;
            RC g = s.get_at(k, sv, &v, sizeof v, &len);
            auto it = before.find(k);
            bool ok = (it == before.end()) ? (g == RC::NOTFOUND) : (g == RC::OK && v == it->second);
            if (!ok && sm++ < 3) std::printf("  [snapview] k=%llu get_at=%s v=%u want=%u\n", (unsigned long long)k, rc_str(g), v, it == before.end() ? 0u : it->second);
        }
        std::printf("snapshot view miss=%d (of 500)\n", sm); if (sm) bad = 1;
        check("after-snapshot-writes", NK);
        RC rb = s.rollback(0);
        std::printf("rollback rc=%s last_commit=%u\n", rc_str(rb), (unsigned)s.last_commit());
        if (rb == RC::OK) { model = before; check("after-rollback", NK); }
        s.forget_snapshots();
    }
    // 重开
    {
        Store s2;
        RC o = s2.open(&d, arena, 512);
        std::printf("reopen rc=%s last_commit=%u runs=%u\n", rc_str(o), (unsigned)s2.last_commit(), s2.run_count());
        if (o != RC::OK) return 2;
        int miss = 0;
        for (auto& kv : model) { u32 v = 0; u16 len = 0; RC g = s2.get(kv.first, &v, sizeof v, &len); if (g != RC::OK || v != kv.second) { if (miss++ < 3) std::printf("  [reopen] k=%llu get=%s v=%u want=%u\n", (unsigned long long)kv.first, rc_str(g), v, kv.second); } }
        std::printf("reopen miss=%d (of %zu)\n", miss, model.size()); if (miss) bad = 1;
        Report r2; RC v2 = s2.verify(&r2);
        std::printf("reopen verify rc=%s err=%u first_err=%s\n", rc_str(v2), r2.errors, r2.first_err);
        if (v2 != RC::OK || r2.errors) bad = 1;
        // 回绕之后再提交
        u32 v = 0xB0000000u; s2.put(1, &v, sizeof v); RC c = s2.commit(true);
        std::printf("commit after reopen rc=%s last_commit=%u\n", rc_str(c), (unsigned)s2.last_commit());
        if (c != RC::OK) return 2;
    }
    std::printf("g12 坑 102 版本回绕（70000 次逻辑提交 + 跨回绕快照）: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
