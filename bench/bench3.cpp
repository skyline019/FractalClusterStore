// bench3.cpp —— v3 (FCS) 写放大/吞吐基准：与 bench_v1 / bench2 完全相同的负载
// 输出一行 CSV: engine,ops,commit_every,total_value_bytes,write_bytes,read_bytes,writes,read_calls,seconds
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>

using namespace fcs;
static u8 arena[512 * PAGE];

int main(int argc, char** argv) {
    int ops = argc > 1 ? atoi(argv[1]) : 20000;
    int commit_every = argc > 2 ? atoi(argv[2]) : 500;
    int group = argc > 3 ? atoi(argv[3]) : 0;      // 组提交：每 group 个逻辑提交落盘一次
    static RamDev dev(8192);
    static Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    if (argc > 4 && atoi(argv[4]) > 0) s.set_lvl_t((u32)atoi(argv[4]));   // 第 12 轮 T1：argv[4] = lvl_t（0 = 默认 8）
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
        if (s.put(k, buf, len) == RC::OK) logical += len;
        if (i % commit_every == commit_every - 1) {
            int b = (i + 1) / commit_every;
            if (group > 1) { if (b % group) s.commit(false); else s.commit(); }
            else s.commit();
        }
    }
    s.commit();
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    u64 wbytes = dev.st.write_bytes, rbytes0 = dev.st.read_bytes, writes0 = dev.st.writes, reads0 = dev.st.read_calls;
    u64 read_ok = 0, read_bad = 0;
    for (int i = 0; i < ops; ++i) {
        u64 k = rng() % 200000 + 1;
        u8 b[64]; u16 l = 0;
        RC g = s.get(k, b, sizeof b, &l);
        if (g == RC::OK) ++read_ok; else if (g == RC::NOTFOUND) ++read_bad;
    }
    std::printf("v3,%d,%d,%llu,%llu,%llu,%llu,%llu,%.3f\n", ops, commit_every,
                (unsigned long long)logical, (unsigned long long)wbytes,
                (unsigned long long)dev.st.read_bytes, (unsigned long long)dev.st.writes,
                (unsigned long long)dev.st.read_calls, sec);
    Report r;
    RC vr = s.verify(&r);
    std::printf("# readback ok=%llu notfound=%llu cells=%u runs=%u recs=%u live=%llu phys=%llu pages=%u/%u errors=%u(%s)\n",
                (unsigned long long)read_ok, (unsigned long long)read_bad, r.cells, r.runs, r.recs,
                (unsigned long long)r.live, (unsigned long long)r.bytes, r.pages,
                s.used_pages(), r.errors, r.errors ? r.first_err : "-");
    std::printf("# breakdown: flush=%llu compact=%llu merge=%llu manifest=%llu\n",
                (unsigned long long)s.wr_flush(), (unsigned long long)s.wr_compact(), (unsigned long long)s.wr_merge(), (unsigned long long)s.wr_man());
    std::printf("# verify=%s write_amp=%.2fx read_ops=%llu probes=%.2f/get seg_max=%u\n", rc_str(vr),
                logical ? (double)wbytes / (double)logical : 0.0,
                (unsigned long long)(dev.st.read_calls - reads0),
                s.gets() ? (double)s.probes() / (double)s.gets() : 0.0, s.seg_max());
    return vr == RC::OK ? 0 : 1;
}
