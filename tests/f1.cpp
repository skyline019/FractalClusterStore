// f1：FCS 基础语义
#include "fcs.h"
#include <cstdio>
#include <map>
#include <random>
using namespace fcs;
static u8 arena[512 * PAGE];

int main() {
    static RamDev dev(2048);
    static Store s;
    RC rc = s.open(&dev, arena, 512);
    if (rc != RC::OK) { std::printf("open %s\n", rc_str(rc)); return 1; }
    u8 buf[256]; u16 len = 0;
    if (s.get(1, buf, sizeof buf, &len) != RC::NOTFOUND) { std::printf("empty get\n"); return 1; }
    s.put(1, "hello", 5);
    if (s.get(1, buf, sizeof buf, &len) != RC::OK || len != 5) { std::printf("uncommitted get\n"); return 1; }
    rc = s.commit();
    if (rc != RC::OK) { std::printf("commit %s\n", rc_str(rc)); return 1; }
    if (s.get(1, buf, sizeof buf, &len) != RC::OK || std::memcmp(buf, "hello", 5)) { std::printf("get after commit\n"); return 1; }
    // 覆盖 + 删除
    s.put(1, "HELLO", 5);
    s.put(2, "two", 3);
    s.commit();
    if (s.get(1, buf, sizeof buf, &len) != RC::OK || std::memcmp(buf, "HELLO", 5)) { std::printf("overwrite\n"); return 1; }
    s.del(2);
    s.commit();
    if (s.get(2, buf, sizeof buf, &len) != RC::NOTFOUND) { std::printf("tombstone\n"); return 1; }
    // 扫描
    u32 n = 0;
    s.scan([&](u64, const u8*, u16) { ++n; return true; });
    std::printf("scan=%u\n", n);
    // 随机批量
    std::mt19937_64 rng(7);
    std::map<u64, std::vector<u8>> oracle;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 500; ++i) {
            u64 k = rng() % 3000 + 1;
            if (rng() % 8 == 0) { s.del(k); oracle.erase(k); continue; }
            u8 v[40];
            u32 x = (u32)rng();
            std::memcpy(v, &x, 4);
            u16 l = (u16)(4 + rng() % 30);
            for (u16 j = 4; j < l; ++j) v[j] = (u8)(x + j);
            s.put(k, v, l);
            oracle[k].assign(v, v + l);
        }
        rc = s.commit();
        if (rc != RC::OK) { std::printf("round %d commit %s\n", round, rc_str(rc)); return 1; }
        u32 bad = 0;
        for (auto& kv : oracle) {
            u8 b2[64]; u16 l2 = 0;
            RC g = s.get(kv.first, b2, sizeof b2, &l2);
            if (g != RC::OK || l2 != kv.second.size() || std::memcmp(b2, kv.second.data(), l2)) {
                if (bad < 3) std::printf("  bad k=%llu rc=%s\n", (unsigned long long)kv.first, rc_str(g));
                ++bad;
            }
        }
        Report rep;
        RC vr = s.verify(&rep);
        std::printf("round %d: bad=%u verify=%s cells=%u segs=%u runs=%u recs=%u bytes=%llu err=%u(%s)\n",
                    round, bad, rc_str(vr), rep.cells, rep.segs, rep.runs, rep.recs,
                    (unsigned long long)rep.bytes, rep.errors, rep.errors ? rep.first_err : "-");
        if (bad || vr != RC::OK) return 1;
    }
    // 重开
    RC ro = s.open(&dev, arena, 512);
    std::printf("reopen %s\n", rc_str(ro));
    u32 bad = 0;
    for (auto& kv : oracle) {
        u8 b2[64]; u16 l2 = 0;
        RC g = s.get(kv.first, b2, sizeof b2, &l2);
        if (g != RC::OK || l2 != kv.second.size() || std::memcmp(b2, kv.second.data(), l2)) ++bad;
    }
    std::printf("after reopen bad=%u scan=%u\n", bad, (unsigned)([&]{ u32 c=0; s.scan([&](u64,const u8*,u16){++c;return true;}); return c; })());
    std::printf(bad ? "FAIL\n" : "PASS\n");
    return bad ? 1 : 0;
}
