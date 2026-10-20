// bench_v2.cpp —— v2 引擎写放大/吞吐基准（与 bench_v1 完全相同的负载）
// 输出一行 CSV: engine,ops,commit_every,total_value_bytes,write_bytes,read_bytes,writes,read_calls,seconds
#include "store.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>

using namespace xdb;
static u8 arena[NAV_CAP * PAGE];

int main(int argc, char** argv) {
    int ops = argc > 1 ? atoi(argv[1]) : 20000;
    int commit_every = argc > 2 ? atoi(argv[2]) : 500;
    static RamDev dev(8192);
    static Engine e;
    if (e.open(&dev, arena, NAV_CAP) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::mt19937_64 rng(12345);
    u64 logical = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ops; ++i) {
        u64 k = rng() % 200000 + 1;
        u8 buf[32];
        u32 v = (u32)rng();
        std::memcpy(buf, &v, 4);
        u16 len = (u16)(4 + rng() % 24);
        for (u16 j = 4; j < len; ++j) buf[j] = (u8)(v + j);
        if (e.put(k, buf, len) == RC::OK) logical += len;
        if (i % commit_every == commit_every - 1) e.commit();
    }
    e.commit();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    u64 read_ok = 0, read_bad = 0;
    for (int i = 0; i < ops; ++i) {
        u64 k = rng() % 200000 + 1;
        u8 b[64]; u16 l = 0;
        RC g = e.get(k, b, sizeof b, &l);
        if (g == RC::OK) ++read_ok; else if (g == RC::NOTFOUND) ++read_bad;
    }
    std::printf("v2,%d,%d,%llu,%llu,%llu,%llu,%llu,%.3f\n", ops, commit_every,
                (unsigned long long)logical, (unsigned long long)dev.st.write_bytes,
                (unsigned long long)dev.st.read_bytes, (unsigned long long)dev.st.writes,
                (unsigned long long)dev.st.read_calls, sec);
    Engine::VerifyReport r;
    e.verify(&r);
    std::printf("# readback ok=%llu notfound=%llu leaves=%u inners=%u pubs=%u orphans=%u errors=%u\n",
                (unsigned long long)read_ok, (unsigned long long)read_bad, r.leaves, r.inners, r.pubs, r.orphans, r.errors);
    return 0;
}
