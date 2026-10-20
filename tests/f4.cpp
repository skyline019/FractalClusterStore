// f4：规模化（50k 写 / 多次提交 / 重开）+ 全量 oracle 对照
#include "fcs.h"
#include <cstdio>
#include <map>
#include <random>
using namespace fcs;
static u8 arena[4096 * PAGE];

int main() {
    static RamDev dev(16384);
    static Store s;
    if (s.open(&dev, arena, 4096) != RC::OK) { std::printf("open\n"); return 1; }
    std::mt19937_64 rng(2026);
    std::map<u64, std::vector<u8>> oracle;
    const int OPS = 50000, COMMIT = 500;
    for (int i = 0; i < OPS; ++i) {
        u64 k = rng() % 40000 + 1;
        if (rng() % 10 == 0) { s.del(k); oracle.erase(k); }
        else {
            u8 v[40]; u32 x = (u32)rng();
            std::memcpy(v, &x, 4);
            u16 l = (u16)(4 + rng() % 30);
            for (u16 j = 4; j < l; ++j) v[j] = (u8)(x * 31 + j);
            s.put(k, v, l);
            oracle[k].assign(v, v + l);
        }
        if (i % COMMIT == COMMIT - 1) {
            RC rc = s.commit();
            if (rc != RC::OK) { std::printf("commit %d: %s\n", i, rc_str(rc)); return 1; }
        }
    }
    if (s.commit() != RC::OK) { std::printf("final commit\n"); return 1; }
    auto check = [&]() {
        u32 bad = 0;
        for (auto& kv : oracle) {
            u8 b[64]; u16 l = 0;
            RC g = s.get(kv.first, b, sizeof b, &l);
            if (g != RC::OK || l != kv.second.size() || std::memcmp(b, kv.second.data(), l)) ++bad;
        }
        u32 n = 0;
        s.scan([&](u64, const u8*, u16) { ++n; return true; });
        return std::pair<u32, u32>(bad, n);
    };
    Report rep;
    RC vr = s.verify(&rep);
    auto r1 = check();
    std::printf("pre-reopen: verify=%s bad=%u scan=%u oracle=%zu cells=%u runs=%u pages=%u/%u\n",
                rc_str(vr), r1.first, r1.second, oracle.size(), rep.cells, rep.runs,
                s.used_pages(), dev.capacity());
    if (r1.first || r1.second != (u32)oracle.size() || vr != RC::OK) { std::printf("FAIL\n"); return 1; }

    RC ro = s.open(&dev, arena, 4096);
    Report rep2;
    RC vr2 = s.verify(&rep2);
    auto r2 = check();
    std::printf("post-reopen: open=%s verify=%s bad=%u scan=%u cells=%u runs=%u\n",
                rc_str(ro), rc_str(vr2), r2.first, r2.second, rep2.cells, rep2.runs);
    if (ro != RC::OK || vr2 != RC::OK || r2.first || r2.second != (u32)oracle.size()) { std::printf("FAIL\n"); return 1; }

    // 重开后再写一轮，验证可继续演进
    for (int i = 0; i < 5000; ++i) {
        u64 k = rng() % 40000 + 1;
        u8 v[40]; u32 x = (u32)rng(); std::memcpy(v, &x, 4);
        u16 l = (u16)(4 + rng() % 30);
        for (u16 j = 4; j < l; ++j) v[j] = (u8)(x * 17 + j);
        s.put(k, v, l);
        oracle[k].assign(v, v + l);
    }
    if (s.commit() != RC::OK) { std::printf("post-reopen commit\n"); return 1; }
    Report rep3;
    RC vr3 = s.verify(&rep3);
    auto r3 = check();
    std::printf("after more writes: verify=%s bad=%u scan=%u oracle=%zu cells=%u\n",
                rc_str(vr3), r3.first, r3.second, oracle.size(), rep3.cells);
    bool ok = vr3 == RC::OK && !r3.first && r3.second == (u32)oracle.size();
    std::printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
