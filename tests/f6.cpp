// f6：run 级存活账必须**精确**（覆盖型负载 + 压实/合并同批发生）
//
// 这是被“热点键反复覆盖”挖出来的坑：
//   - 压实产物与合并产物可能同 seq（都取源 run 的最大 seq），谁是赢家要由 (seq, rid) 全序定；
//   - 合并后的 run 只有一个 seq，若用它的 seq 反过来判"每条记录的旧副本"，会把老记录算成新的；
//   - 段在切分/摘链时若不把存活计数退回 run，recount 会重复累加。
// 只要有一处不精确，verify() 的 "run live recs" 立刻报错（这里每提交一次就查一次），
// 空间也会因为"整条 run 全死也不敢回收"而缓慢膨胀。
#include "fcs.h"
#include <cstdio>
#include <map>
#include <random>
#include <vector>
using namespace fcs;

static int one(u32 nk, u32 ops_per_pass, u32 ce, u32 passes, u64 seed) {
    static u8 arena[1024 * PAGE];
    RamDev dev(16384);
    Store s;
    if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("open fail\n"); return 2; }
    std::mt19937_64 rng(seed);
    std::map<u64, std::vector<u8>> model;
    u32 commits = 0, mism = 0;
    for (u32 p = 0; p < passes; ++p) {
        for (u32 i = 0; i < ops_per_pass; ++i) {
            u64 k = rng() % nk + 1;
            std::vector<u8> v(4 + rng() % 37);
            u32 x = (u32)rng();
            std::memcpy(v.data(), &x, 4);
            for (size_t j = 4; j < v.size(); ++j) v[j] = (u8)(x + j);
            if (s.put(k, v.data(), (u16)v.size()) != RC::OK) { std::printf("put fail\n"); return 2; }
            model[k] = v;
            if (i % ce == ce - 1) {
                RC rc = s.commit();
                if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); return 2; }
                ++commits;
                Report rep;
                if (s.verify(&rep) != RC::OK) {
                    std::printf("verify 失败：第 %u 次提交 errs=%u (%s) key=%llu\n", commits,
                                rep.errors, rep.first_err ? rep.first_err : "-",
                                (unsigned long long)rep.first_key);
                    return 1;
                }
            }
        }
    }
    // 收尾：全量对拍 + 归一 + 重开
    for (auto& kv : model) {
        u8 b[64]; u16 l = 0;
        if (s.get(kv.first, b, sizeof b, &l) != RC::OK || l != kv.second.size() ||
            std::memcmp(b, kv.second.data(), l) != 0) ++mism;
    }
    Report rep;
    RC vr = s.verify(&rep);
    u32 used1 = s.used_pages();
    if (s.consolidate(2) != RC::OK) { std::printf("consolidate fail\n"); return 2; }
    Report rep2;
    if (s.verify(&rep2) != RC::OK) {
        std::printf("归一后 verify 失败：%s\n", rep2.first_err ? rep2.first_err : "-");
        return 1;
    }
    std::printf("  键空间 %u / 每轮 %u 次 / 每 %u 次提交：%u 次提交，%zu 键，bad=%u，used %u 页\n",
                nk, ops_per_pass, ce, commits, model.size(), mism, used1);
    return (vr == RC::OK && mism == 0) ? 0 : 1;
}

int main() {
    // 三种形状：极热（2000 键，大量覆盖）、窄（300 键，几乎每键每轮死一次）、中（20000 键）
    if (one(2000, 4000, 200, 3, 7) != 0) return 1;
    if (one(300, 3000, 100, 4, 11) != 0) return 1;
    if (one(20000, 4000, 400, 2, 13) != 0) return 1;
    std::printf("PASS\n");
    return 0;
}
