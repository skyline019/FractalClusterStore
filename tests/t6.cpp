#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
int main(int argc, char** argv) {
    u32 seed = argc > 1 ? atoi(argv[1]) : 7;
    RamDev dev(8192);
    Engine e;
    e.open(&dev, arena, NAV_CAP);
    std::mt19937_64 rng(seed);
    std::map<u64, u32> oracle;
    for (int i = 0; i < 4600; ++i) {
        u64 k = rng() % 200000 + 1;
        bool isd = (rng() % 10 == 0);
        if (isd) { e.del(k); oracle.erase(k); }
        else {
            u32 v = (u32)rng();
            u8 b[32]; std::memcpy(b, &v, 4);
            u16 len = (u16)(4 + rng() % 24);
            for (u16 j = 4; j < len; ++j) b[j] = (u8)(v + j);
            if (e.put(k, b, len) == RC::OK) oracle[k] = v;
        }
        // 立即回读
        u8 rb[64]; u16 rl = 0;
        RC g = e.get(k, rb, sizeof rb, &rl);
        if (isd) {
            if (g == RC::OK) { std::printf("i=%d DEL not visible k=%llu\n", i, (unsigned long long)k); return 1; }
        } else {
            if (g != RC::OK) { std::printf("i=%d PUT not visible k=%llu rc=%s\n", i, (unsigned long long)k, rc_str(g)); return 1; }
            u32 got; std::memcpy(&got, rb, 4);
            if (got != oracle[k]) { std::printf("i=%d PUT wrong k=%llu got=%u want=%u\n", i, (unsigned long long)k, got, oracle[k]); return 1; }
        }
        if (i % 500 == 499) e.commit();
    }
    e.commit();
    std::printf("all ops ok; oracle=%zu\n", oracle.size());
    u32 cnt = 0; e.scan([&](u64, const u8*, u16) { ++cnt; return true; });
    std::printf("scan=%u\n", cnt);
    // 列出所有叶页及其链接
    u32 nleaf = 0, total = 0, sum = 0;
    for (u16 p2 = 2; p2 < dev.capacity(); ++p2) {
        const u8* b = dev.raw(p2);
        if (!page_ok(b) || page_kind(b) != PK_LEAF) continue;
        LHdr h; lh_read(b, h);
        u32 ns = rd32(b + 4);
        u32 np = 0;
        for (u32 unit = 0; unit < ns;) {
            u32 off = slot_off(unit);
            u8 k2 = b[off];
            u32 u = slot_units(k2);
            if (unit + u > ns) break;
            if (k2 == SK_PUB) ++np;
            unit += u;
        }
        ++nleaf; total += np;
        std::printf("  P%u min=%llu prev=%u next=%u pubs=%u nslots=%u\n", p2, (unsigned long long)h.min_key, h.prev, h.next, np, ns);
    }
    std::printf("leaves=%u pubs=%u\n", nleaf, total);
    return cnt == oracle.size() ? 0 : 1;
}
