// 规模测试：深树/多层内页 + 回退 + 崩溃恢复
#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
static int failures = 0;
static void chk(bool ok, const char* what) { if (!ok) { std::printf("  FAIL: %s\n", what); ++failures; } }

static u32 check_oracle(Engine& e, std::map<u64, u32>& oracle) {
    u32 bad = 0;
    for (auto& kv : oracle) {
        u8 buf[1024]; u16 len = 0;
        RC g = e.get(kv.first, buf, sizeof buf, &len);
        u32 v = 0; if (g == RC::OK && len >= 4) std::memcpy(&v, buf, 4);
        if (g != RC::OK || v != kv.second) {
            if (bad < 3) std::printf("   bad k=%llu rc=%s v=%u want=%u\n", (unsigned long long)kv.first, rc_str(g), v, kv.second);
            ++bad;
        }
    }
    return bad;
}


// 手动下降到叶并打印该页信息（调试）
static void dump_route(RamDev& dev, u16 root, u64 key) {
    u16 pid = root;
    for (int d = 0; d < 10; ++d) {
        const u8* b = dev.raw(pid);
        if (page_kind(b) == PK_LEAF) break;
        IHdr ih; ih_read(b, ih);
        u32 lo = 0, hi = ih.nslots;
        while (lo + 1 < hi) {
            u32 mid = (lo + hi) / 2; ISlot s; islot_ok(b, mid, s);
            if (s.key <= key) lo = mid; else hi = mid;
        }
        ISlot s; islot_ok(b, lo, s);
        std::printf("   inner P%u level=%u nslots=%u -> child %u (key=%llu)\n", pid, ih.level, ih.nslots, s.child, (unsigned long long)s.key);
        pid = s.child;
    }
    const u8* b = dev.raw(pid);
    LHdr h; lh_read(b, h);
    std::printf("   leaf P%u min=%llu prev=%u next=%u\n", pid, (unsigned long long)h.min_key, h.prev, h.next);
    u32 ns = rd32(b + 4);
    for (u32 unit = 0; unit < ns;) {
        u32 off = slot_off(unit); u8 k2 = b[off]; u32 u2 = slot_units(k2);
        if (unit + u2 > ns) break;
        if (k2 == SK_PUB) { Pub pp; if (pub_at(b, off, pp) && pp.key == key) { u32 vv = 0; if (pp.r_off) std::memcpy(&vv, b + pp.r_off + VALHDR_SZ, 4); std::printf("     pub ver=%u val=%u\n", pp.ver, vv); } }
        unit += u2;
    }
}

int main() {
    RamDev dev(8192);
    Engine e;
    RC rc = e.open(&dev, arena, NAV_CAP);
    if (rc != RC::OK) return 1;
    std::mt19937_64 rng(7);
    std::map<u64, u32> oracle;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        u64 k = rng() % 200000 + 1;
        if (rng() % 10 == 0) { e.del(k); oracle.erase(k); }
        else {
            u32 v = (u32)rng();
            u8 buf[32]; std::memcpy(buf, &v, 4);
            u16 len = (u16)(4 + rng() % 24);
            for (u16 j = 4; j < len; ++j) buf[j] = (u8)(v + j);
            rc = e.put(k, buf, len);
            if (rc == RC::OK) oracle[k] = v;
            else if (rc != RC::FULL) { std::printf("put rc=%s i=%d\n", rc_str(rc), i); return 1; }
        }
        if (i % 500 == 499) {
            rc = e.commit();
            if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); return 1; }
            u32 c0 = 0; e.scan([&](u64, const u8*, u16) { ++c0; return true; });
            if (c0 != oracle.size()) {
                std::printf("LOST at i=%d scan=%u oracle=%zu\n", i, c0, oracle.size());
                Engine::VerifyReport rr; e.verify(&rr);
                std::printf("  verify err=%u first=%u(%s) leaves=%u inners=%u pubs=%u orphans=%u\n",
                            rr.errors, rr.first_err_page, rr.first_err, rr.leaves, rr.inners, rr.pubs, rr.orphans);
                return 1;
            }
        }
    }
    e.commit();
    Engine::VerifyReport rep;
    rc = e.verify(&rep);
    std::printf("scale: pages=%u leaves=%u inners=%u pubs=%u orphans=%u err=%u(%s)\n",
                rep.pages, rep.leaves, rep.inners, rep.pubs, rep.orphans, rep.errors, rep.first_err);
    chk(rc == RC::OK, "verify");
    chk(check_oracle(e, oracle) == 0, "oracle");
    u32 sc = 0; e.scan([&](u64, const u8*, u16) { ++sc; return true; });
    std::printf("  keys=%zu scan=%u leaves=%u\n", oracle.size(), sc, rep.leaves);
    chk(sc == oracle.size(), "scan count");

    // ---- 快照 / 回退
    std::map<u64, u32> snap_oracle = oracle;
    rc = e.snapshot();
    chk(rc == RC::OK, "snapshot");
    Vers snapv = e.last_commit() + 1;
    std::printf("  snapshot at vers=%u (open=%u) snaps=%u\n", snapv, e.open_vers(), e.snapshot_count());
    for (int i = 0; i < 2000; ++i) {
        u64 k = rng() % 200000 + 1;
        u32 v = (u32)rng();
        u8 buf[8]; std::memcpy(buf, &v, 4);
        if (e.put(k, buf, 4) == RC::OK) oracle[k] = v;
    }
    e.commit();
    chk(check_oracle(e, oracle) == 0, "post-snapshot writes");
    rc = e.rollback(0);
    chk(rc == RC::OK, "rollback");
    u32 bad = check_oracle(e, snap_oracle);
    if (bad) {
        std::printf("  rollback: %u keys wrong  vers_=%u open_=%u\n", bad, e.last_commit(), e.open_vers());
        // 打印若干错误键的所有 pub
        u32 shown = 0;
        for (auto& kv : snap_oracle) {
            u8 b[128]; u16 l = 0;
            RC g = e.get(kv.first, b, sizeof b, &l);
            u32 v = 0; if (g == RC::OK) std::memcpy(&v, b, 4);
            if (g == RC::OK && v == kv.second) continue;
            if (shown++ >= 3) break;
            std::printf("  key %llu want v=%u got rc=%s v=%u:\n", (unsigned long long)kv.first, kv.second, rc_str(g), v);
            dump_route(dev, e.root(), kv.first);
            for (u16 p2 = 2; p2 < dev.capacity(); ++p2) {
                const u8* pg = dev.raw(p2);
                if (!page_ok(pg) || page_kind(pg) != PK_LEAF) continue;
                u32 ns = rd32(pg + 4);
                for (u32 unit = 0; unit < ns;) {
                    u32 off = slot_off(unit);
                    u8 k2 = pg[off];
                    u32 u2 = slot_units(k2);
                    if (unit + u2 > ns) break;
                    if (k2 == SK_PUB) {
                        Pub pp;
                        if (pub_at(pg, off, pp) && pp.key == kv.first) {
                            u32 vv = 0;
                            if (pp.r_off) std::memcpy(&vv, pg + pp.r_off + VALHDR_SZ, 4);
                            std::printf("     P%u ver=%u r_off=%u val=%u\n", p2, pp.ver, pp.r_off, vv);
                        }
                    }
                    unit += u2;
                }
            }
        }
    }
    chk(bad == 0, "rollback oracle");
    // 回退后：检查叶链是否有跨页重复键
    {
        std::vector<u16> ch;
        u16 pid = 0xFFFF;
        // 用 root 下降到最左
        pid = e.root();
        for (int d = 0; d < 20; ++d) {
            const u8* b = dev.raw(pid);
            if (page_kind(b) == PK_LEAF) break;
            IHdr ih; ih_read(b, ih);
            ISlot s0; islot_read(b, 0, s0);
            pid = s0.child;
        }
        u64 prevmax = 0; bool have = false;
        u32 dups = 0;
        for (int g = 0; g < 100000 && pid != 0xFFFF; ++g) {
            const u8* b = dev.raw(pid);
            LHdr h; lh_read(b, h);
            u32 ns = rd32(b + 4);
            // 收集本页键（去重）
            std::vector<u64> ks;
            for (u32 unit = 0; unit < ns;) {
                u32 off = slot_off(unit); u8 k2 = b[off]; u32 u2 = slot_units(k2);
                if (unit + u2 > ns) break;
                if (k2 == SK_PUB) { Pub pp; if (pub_at(b, off, pp) && pp.ver <= e.open_vers()) ks.push_back(pp.key); }
                unit += u2;
            }
            std::sort(ks.begin(), ks.end()); ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
            if (!ks.empty()) {
                if (have && ks[0] <= prevmax) {
                    if (dups < 5) std::printf("   DUP? P%u first=%llu prevmax=%llu\n", pid, (unsigned long long)ks[0], (unsigned long long)prevmax);
                    ++dups;
                }
                prevmax = ks.back(); have = true;
            }
            pid = h.next;
        }
        std::printf("  chain dup pages=%u\n", dups);
    }
    // 回退后仍可写
    for (auto& kv : snap_oracle) { (void)kv; break; }
    for (int i = 0; i < 500; ++i) {
        if (i % 25 == 0) {
            u16 pid2 = e.root();
            for (int d = 0; d < 20; ++d) {
                const u8* b = dev.raw(pid2);
                if (page_kind(b) == PK_LEAF) break;
                IHdr ih; ih_read(b, ih); ISlot s0; islot_read(b, 0, s0); pid2 = s0.child;
            }
            u64 pm = 0; bool hv = false; u32 d2 = 0;
            for (int g = 0; g < 100000 && pid2 != 0xFFFF; ++g) {
                const u8* b = dev.raw(pid2);
                LHdr h; lh_read(b, h);
                u32 ns = rd32(b + 4);
                std::vector<u64> ks;
                for (u32 unit = 0; unit < ns;) {
                    u32 off = slot_off(unit); u8 k2 = b[off]; u32 u2 = slot_units(k2);
                    if (unit + u2 > ns) break;
                    if (k2 == SK_PUB) { Pub pp; if (pub_at(b, off, pp) && pp.r_off && pp.ver <= e.open_vers()) ks.push_back(pp.key); }
                    unit += u2;
                }
                std::sort(ks.begin(), ks.end()); ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
                if (!ks.empty()) {
                    if (hv && ks[0] <= pm) ++d2;
                    pm = ks.back(); hv = true;
                }
                pid2 = h.next;
            }
            if (d2) { std::printf("   BOUNDARY DUP at op %d: %u pages\n", i, d2); break; }
        }
        u64 k = rng() % 1000 + 1;
        u32 v = (u32)rng();
        u8 buf[8]; std::memcpy(buf, &v, 4);
        if (e.put(k, buf, 4) == RC::OK) snap_oracle[k] = v;
    }
    e.commit();
    chk(check_oracle(e, snap_oracle) == 0, "write after rollback");
    rc = e.verify(&rep);
    std::printf("  after rollback: err=%u(%s) firstpage=%u\n", rep.errors, rep.first_err, rep.first_err_page);
    chk(rc == RC::OK, "verify after rollback");

    // ---- 未提交事务 abort
    std::map<u64, u32> before = snap_oracle;
    for (int i = 0; i < 300; ++i) {
        u64 k = rng() % 500 + 1;
        u32 v = 0xDEAD0000 + (u32)i;
        e.put(k, &v, 4);
    }
    e.abort();
    chk(check_oracle(e, before) == 0, "abort");

    // ---- 崩溃恢复：提交后立刻「断电」重开
    for (int r = 0; r < 20; ++r) {
        for (int i = 0; i < 100; ++i) {
            u64 k = rng() % 3000 + 1;
            u32 v = (u32)rng();
            e.put(k, &v, 4);
            if (e.get(k, (void*)&v, 4, nullptr) == RC::OK) before[k] = v;
        }
        e.commit();
    }
    // 重开前：把内页槽序列落成文本，便于对比
    auto dump_inners = [&](const char* tag) {
        FILE* f = std::fopen(tag, "w");
        for (u16 p2 = 2; p2 < dev.capacity(); ++p2) {
            const u8* b = dev.raw(p2);
            if (!page_ok(b) || page_kind(b) != PK_INNER) continue;
            IHdr ih; ih_read(b, ih);
            std::fprintf(f, "P%u level=%u nslots=%u nid=%u footvers=?\n", p2, ih.level, ih.nslots, ih.nid);
            for (u32 i = 0; i < ih.nslots; ++i) {
                ISlot sl;
                if (!islot_ok(b, i, sl)) { std::fprintf(f, "  [%u] BAD\n", i); continue; }
                std::fprintf(f, "  [%u] key=%llu child=%u\n", i, (unsigned long long)sl.key, sl.child);
            }
        }
        std::fclose(f);
    };
    dump_inners("/tmp/inner_before.txt");
    Engine e2;
    rc = e2.open(&dev, arena, NAV_CAP);
    chk(rc == RC::OK, "reopen");
    dump_inners("/tmp/inner_after.txt");
    Engine::VerifyReport rep2;
    rc = e2.verify(&rep2);
    std::printf("  reopen: err=%u(%s) pages=%u firstpage=%u\n", rep2.errors, rep2.first_err, rep2.pages, rep2.first_err_page);
    chk(rc == RC::OK, "verify reopen");
    std::printf("FAILURES: %d\n", failures);
    return failures ? 1 : 0;
}
