// 逐步一致性检查：每次写操作后比较「树可达叶集合」与「叶链集合」
#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
int main(int argc, char** argv) {
    u32 seed = argc > 1 ? (u32)atoi(argv[1]) : 2;
    int rounds = argc > 2 ? atoi(argv[2]) : 1;
    std::mt19937_64 rng(seed);
    static RamDev dev(4096);
    static Engine e;
    if (e.open(&dev, arena, NAV_CAP) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::map<u64, u32> oracle;
    size_t last_leaks = 0;
    for (int round = 0; round < rounds; ++round) {
        for (int i = 0; i < 400; ++i) {
            u64 k = rng() % 500 + 1;
            u32 op = (u32)(rng() % 100);
            RC rc = RC::OK;
            const char* what = "put";
            if (op < 25) { rc = e.del(k); what = "del"; if (rc == RC::OK) oracle.erase(k); }
            else {
                u32 v = (u32)rng();
                u16 len = (u16)(4 + (rng() % 40));
                u8 buf[64];
                std::memcpy(buf, &v, 4);
                for (u16 j = 4; j < len; ++j) buf[j] = (u8)(j * 31 + v);
                rc = e.put(k, buf, len);
                if (rc == RC::OK) oracle[k] = v;
            }
            std::vector<u16> miss, ext, lk;
            e.debug_leaks(lk);
            if (lk.size() > last_leaks) {
                std::printf("LEAK at round %d op %d k=%llu %s rc=%s: ", round, i, (unsigned long long)k, what, rc_str(rc));
                for (u16 q : lk) std::printf("P%u ", q);
                std::printf("\n");
                last_leaks = lk.size();
                if (last_leaks > 2) return 1;
            }
            bool ok = e.debug_chain_tree(miss, ext);
            if (!ok || rc != RC::OK) {
                std::printf("round %d op %d k=%llu %s rc=%s tree-miss=%zu chain-extra=%zu\n", round, i,
                            (unsigned long long)k, what, rc_str(rc), miss.size(), ext.size());
                for (size_t q = 0; q < miss.size() && q < 6; ++q) std::printf("   reachable-not-in-chain P%u\n", miss[q]);
                for (size_t q = 0; q < ext.size() && q < 6; ++q) std::printf("   chain-not-reachable  P%u\n", ext[q]);
                return 1;
            }
            if (i % 37 == 36) {
                rc = e.commit();
                if (rc != RC::OK) { std::printf("commit rc=%s at %d\n", rc_str(rc), i); return 1; }
                std::vector<u16> m2, e2;
                if (!e.debug_chain_tree(m2, e2)) {
                    std::printf("  after commit at op %d: tree-miss=%zu chain-extra=%zu\n", i, m2.size(), e2.size());
                    for (size_t q = 0; q < m2.size() && q < 6; ++q) std::printf("    reachable-not-in-chain P%u\n", m2[q]);
                    for (size_t q = 0; q < e2.size() && q < 6; ++q) std::printf("    chain-not-reachable  P%u\n", e2[q]);
                }
            }
        }
        RC rc = e.commit();
        if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); return 1; }
        Engine::VerifyReport r; e.verify(&r);
        std::printf("round %d leaves=%u inners=%u orphans=%u errors=%u\n", round, r.leaves, r.inners, r.orphans, r.errors);
    }
    std::printf("PASS\n");
    return 0;
}
