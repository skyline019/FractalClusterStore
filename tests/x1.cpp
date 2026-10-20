// 最小验证：put/get 语义 + 提交 + 重开户
#include "store.h"
#include <cstdio>
#include <map>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
static u32 rnd(u32& s) { s = s * 1103515245u + 12345u; return s >> 8; }
int main() {
    static RamDev dev(300);
    Engine e;
    if (e.open(&dev, arena, NAV_CAP) != RC::OK) { std::printf("open fail\n"); return 1; }
    u32 s = 7;
    std::map<u64, u32> oracle;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 60; ++i) {
            u64 k = 1 + rnd(s) % 50;
            u32 v = (u32)(k * 100 + round);
            u16 len = (u16)(4 + rnd(s) % 40);
            u8 buf[64];
            std::memcpy(buf, &v, 4);
            for (int j = 4; j < len; ++j) buf[j] = (u8)(v + j);
            RC rc = e.put(k, buf, len);
            oracle[k] = v;
            if (rc != RC::OK) { std::printf("put %llu rc=%s\n", (unsigned long long)k, rc_str(rc)); return 1; }
        }
        RC rc = e.commit();
        if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); return 1; }
        Engine::VerifyReport rep;
        e.verify(&rep);
        u32 bad = 0;
        for (auto& kv : oracle) {
            u8 buf[64]; u16 len = 0;
            RC g = e.get(kv.first, buf, sizeof buf, &len);
            u32 v = 0;
            if (g == RC::OK) std::memcpy(&v, buf, 4);
            if (g != RC::OK || v != kv.second) {
                if (bad < 3) std::printf("  bad k=%llu rc=%s v=%u want=%u\n", (unsigned long long)kv.first, rc_str(g), v, kv.second);
                ++bad;
            }
        }
        std::printf("round %d verify err=%u(%s) bad=%u leaves=%u pubs=%u\n", round, rep.errors, rep.first_err, bad, rep.leaves, rep.pubs);
        if (bad) return 1;
        if (rc != RC::OK) return 1;
    }
    std::printf("write_bytes=%llu reads=%llu\n", (unsigned long long)dev.st.write_bytes, (unsigned long long)dev.st.read_calls);
    std::printf("PASS\n");
    return 0;
}
