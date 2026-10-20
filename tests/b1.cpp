// b1：大库（> 256 MB）。曾经 PageId 是 u16 ⇒ 单库上限 65535 页 = 256 MB，
// 而且超了之后 RamDev 的容量会**静默回绕**成 200000&0xFFFF = 3392 页。
//
// 两段负载，覆盖两种"大页号"形态：
//   A) 66000 × 3900 B —— 3.9 KB 值现在走**溢出链**（1 条链 + 1 个数据页），
//      于是 page0 会被推到 65535 以上，sp 的 `页<<12|偏移` 打包必须不截断；
//   B) 30000 × 2900 B —— 内联大值，一条 run ≈ 1500 页，测试"大 run + meta 页"
//      在 32 位页号下的行为（贴在 page0+npages 的 meta 也在 65535 之上）。
// 最后抽查**中点之后**的键：那正是旧代码把 packed>>16 截断成余数时会静默读错的地方。
#include "fcs.h"
#include <cstdio>
#include <chrono>
#include <cstring>
#include <vector>
#include <cstdlib>
using namespace fcs;
static u64 now_ns() { return (u64)std::chrono::steady_clock::now().time_since_epoch().count(); }
int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 66000;
    int M = argc > 2 ? atoi(argv[2]) : 30000;
    // 库有 ~1.5 GB：默认落在当前目录（/tmp 常常是 tmpfs，装不下）。
    const char* path = getenv("FCS_BIGDB") ? getenv("FCS_BIGDB") : "build/big2.db";
    std::remove(path);
    u64 t0 = now_ns();
    u32 used = 0, runs = 0;
    {
        FileDev dev(path, 400000);                 // 400000 页 = 1.56 GB（以前会被截成 3392）
        Store s; if (s.open(&dev, nullptr, 2048) != RC::OK) { printf("open fail\n"); return 1; }
        std::vector<u8> v(3900, 0x5A);
        for (int i = 0; i < N; ++i) {
            std::memcpy(v.data(), &i, 4);
            RC rc = s.put((u64)i + 1, v.data(), (u16)v.size());
            if (rc != RC::OK) { printf("put fail @%d rc=%s\n", i, rc_str(rc)); return 1; }
            if (i % 2000 == 1999) { rc = s.commit(); if (rc != RC::OK) { printf("commit fail @%d rc=%s used=%u\n", i, rc_str(rc), s.used_pages()); return 1; } }
        }
        if (s.commit() != RC::OK) { printf("final commit fail\n"); return 1; }
        used = s.used_pages(); runs = s.run_count();
        Report r; s.verify(&r);
        // used = 文件高水位（含合并/压实留下的、已被回收但还在高水位以下的页）；
        // r.pages = verify 算出来的**活页数**。两个数差得远说明碎片多，不是数据多。
        printf("A: %d×3900B（溢出链）device max=%u 高水位=%u (%.0f MB) 活页=%llu (%.0f MB) runs=%u verify=%u(%s)\n",
               N, (u32)dev.max_pages, used, used * 4096.0 / 1048576.0,
               (unsigned long long)s.live_page_count(), s.live_page_count() * 4096.0 / 1048576.0, runs,
               r.errors, r.errors ? r.first_err : "-");
        if (r.errors) return 1;
    }
    {   // 段 B：内联大值的大 run（键域与 A 不重叠，避免互相覆盖）
        FileDev dev(path, 400000);
        Store s; if (s.open(&dev, nullptr, 2048) != RC::OK) { printf("openB fail\n"); return 1; }
        std::vector<u8> v(2900, 0x33);
        for (int i = 0; i < M; ++i) {
            std::memcpy(v.data(), &i, 4);
            if (s.put((u64)1000000 + i, v.data(), (u16)v.size()) != RC::OK) { printf("putB fail @%d\n", i); return 1; }
            if (i % 5000 == 4999) {
                if (s.commit() != RC::OK) { printf("commitB fail\n"); return 1; }
                            }
        }
        if (s.commit() != RC::OK) { printf("final commitB fail\n"); return 1; }
        used = s.used_pages(); runs = s.run_count();
        Report r; s.verify(&r);
        printf("B: %d×2900B（内联）高水位=%u (%.0f MB) 活页=%llu (%.0f MB) runs=%u verify=%u(%s)\n",
               M, used, used * 4096.0 / 1048576.0, (unsigned long long)s.live_page_count(),
               s.live_page_count() * 4096.0 / 1048576.0, runs, r.errors, r.errors ? r.first_err : "-");
        if (r.errors) return 1;
    }
    {
        FileDev dev(path, 400000);
        u64 o0 = now_ns();
        Store s; if (s.open(&dev, nullptr, 2048) != RC::OK) { printf("reopen fail\n"); return 1; }
        u64 o1 = now_ns();
        Report r; s.verify(&r);
        u64 o2 = now_ns();
        // 抽查：两段的中点之后（页号 > 65535 的区间）
        int bad = 0;
        for (int i = N / 2; i < N; i += 97) {
            u8 b[8]; u16 l = 0;
            if (s.get((u64)i + 1, b, sizeof b, &l) != RC::OK) { ++bad; continue; }
            int got; std::memcpy(&got, b, 4);
            if (got != i) ++bad;
        }
        for (int i = M / 2; i < M; i += 53) {
            u8 b[8]; u16 l = 0;
            if (s.get((u64)1000000 + i, b, sizeof b, &l) != RC::OK) { ++bad; continue; }
            int got; std::memcpy(&got, b, 4);
            if (got != i) ++bad;
        }
        // compact_file：把 503 MB 的高水位真的还回去（容量改造之后这条路径同样要成立）
        u32 hi0 = s.used_pages();
        RC crc = s.compact_file();
        u64 live = s.live_page_count();       // 高水位 − 空闲（含溢出链）
        printf("compact_file: rc=%s  高水位 %u → %u（活页 %llu，含链）\n", rc_str(crc), hi0, s.used_pages(),
               (unsigned long long)live);
        if (crc != RC::OK || s.used_pages() > live + live / 2 + 64) { printf("  FAIL compact_file 没把高水位收回来\n"); return 1; }
        Report r4; s.verify(&r4);
        if (r4.errors) { printf("  FAIL compact 后 verify: %s\n", r4.first_err); return 1; }
        printf("reopen %.1f ms  verify %.1f ms  errors=%u 回读坏=%d meta_hits=%llu runs=%u\n",
               (o1 - o0) / 1e6, (o2 - o1) / 1e6, r.errors, bad,
               (unsigned long long)s.meta_hits(), s.run_count());
        if (bad || r.errors) { printf("FAILURES\n"); return 1; }
    }
    printf("total %.1f s\nPASS\n", (now_ns() - t0) / 1e9);
    return 0;
}
