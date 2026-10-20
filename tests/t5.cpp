#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
int main(int argc, char** argv) {
    u32 seed = argc > 1 ? atoi(argv[1]) : 1;
    RamDev dev(4096);
    Engine e;
    RC rc = e.open(&dev, arena, NAV_CAP);
    if (rc != RC::OK) { std::printf("open %s\n", rc_str(rc)); return 1; }
    std::mt19937_64 rng(seed);
    std::map<u64, u32> oracle;
    for (u32 i = 1; i <= 4000; ++i) {
        u64 k = rng() % 300 + 1;
        if (rng() % 8 == 0) { rc = e.del(k); if (rc == RC::OK) oracle.erase(k); }
        else {
            u32 v = (u32)rng();
            u8 buf[32]; std::memcpy(buf, &v, 4);
            u16 len = (u16)(4 + rng() % 20);
            for (u16 j = 4; j < len; ++j) buf[j] = (u8)j;
            rc = e.put(k, buf, len);
            if (rc == RC::OK) oracle[k] = v;
        }
        if (rc != RC::OK && rc != RC::FULL) { std::printf("op %u rc=%s\n", i, rc_str(rc)); return 1; }
        if (i % 100 == 0) {
            rc = e.commit();
            if (rc != RC::OK) { std::printf("commit rc=%s at %u\n", rc_str(rc), i); return 1; }
            u32 bad = 0;
            for (auto& kv : oracle) {
                u8 b[64]; u16 l = 0;
                RC g = e.get(kv.first, b, sizeof b, &l);
                u32 v = 0; if (g == RC::OK) std::memcpy(&v, b, 4);
                if (g != RC::OK || v != kv.second) {
                    if (bad < 3) std::printf("  i=%u k=%llu rc=%s v=%u want=%u\n", i, (unsigned long long)kv.first, rc_str(g), v, kv.second);
                    ++bad;
                }
            }
            u32 cnt = 0; e.scan([&](u64, const u8*, u16) { ++cnt; return true; });
            if (bad || cnt != oracle.size()) {
                std::printf("LOST i=%u bad=%u scan=%u oracle=%zu\n", i, bad, cnt, oracle.size());
                Engine::VerifyReport r; e.verify(&r);
                std::printf("  verify err=%u first=%u(%s) leaves=%u inners=%u pubs=%u orphans=%u\n",
                            r.errors, r.first_err_page, r.first_err, r.leaves, r.inners, r.pubs, r.orphans);
                return 1;
            }
        }
    }
    std::printf("ok\n");
    return 0;
}
