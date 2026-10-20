// bench5.cpp —— 第 5 轮新增能力的可复现数字：
//   ./bench5 big       大值（A2 溢出页）写/读吞吐 + 空间开销
//   ./bench5 scan      扫描预读（B2）在 FileDev 上的 pread 次数
//   ./bench5 mt N T    多写者（B3）N 线程 T 次提交的墙钟时间
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <cstdlib>
using namespace fcs;

static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int do_big(u32 val_bytes, int nkeys) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench5_big.db"; std::remove(P);
    FileDev fd(P, 400000);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::vector<u8> v(val_bytes);
    for (u32 i = 0; i < val_bytes; ++i) v[i] = (u8)(i * 31 + 7);
    double t0 = now_s();
    for (int i = 0; i < nkeys; ++i) {
        v[0] = (u8)i;
        if (s.put((u64)i + 1, v.data(), (u16)val_bytes) != RC::OK) { std::printf("put fail\n"); return 1; }
    }
    s.commit();
    double t1 = now_s();
    u64 wb = fd.st.write_bytes;
    // 读回来（随机序，避免页缓存顺序带来的乐观偏差）
    std::vector<u8> out(val_bytes);
    std::vector<int> ord(nkeys);
    for (int i = 0; i < nkeys; ++i) ord[i] = i;
    std::mt19937 rng(7);
    std::shuffle(ord.begin(), ord.end(), rng);
    u64 ok = 0, bad = 0;
    double t2 = now_s();
    for (int i : ord) {
        u16 l = 0;
        if (s.get((u64)i + 1, out.data(), (u16)out.size(), &l) != RC::OK || l != val_bytes) { ++bad; continue; }
        for (u32 j = 1; j < val_bytes; ++j)
            if (out[j] != (u8)(j * 31 + 7)) { ++bad; break; }
        ++ok;
    }
    double t3 = now_s();
    u64 rb = fd.st.read_bytes;
    Report r;
    s.verify(&r);
    double mbs_w = (double)wb / (t1 - t0) / 1e6, mbs_r = (double)rb / (t3 - t2) / 1e6;
    std::printf("value=%u B keys=%d 写 %.1f MB in %.3f s = %.0f MB/s | 读 %.1f MB in %.3f s = %.0f MB/s"
                " | ok=%llu bad=%llu used=%u pages ovfl=%u verify=%s\n",
                val_bytes, nkeys, (double)wb / 1e6, t1 - t0, mbs_w, (double)rb / 1e6, t3 - t2, mbs_r,
                (unsigned long long)ok, (unsigned long long)bad, s.used_pages(), r.ovfl,
                r.errors ? r.first_err : "OK");
    return bad || r.errors ? 1 : 0;
}

// 大值顺序读：游标每个值都走 read_ovfl（链按 8 页一批 read_multi）
static int do_bigscan(int arena_pages, int nkeys, u32 vb) {
    static std::vector<u8> arena;
    arena.assign((size_t)arena_pages * PAGE, 0);
    const char* P = "build/bench5_bigscan.db"; std::remove(P);
    FileDev fd(P, 400000);
    {
        Store s;
        if (s.open(&fd, arena.data(), arena_pages) != RC::OK) return 1;
        std::vector<u8> v(vb, 0x5A);
        for (int i = 0; i < nkeys; ++i) {
            v[0] = (u8)i;
            s.put((u64)i + 1, v.data(), (u16)vb);
            if (i % 250 == 249) s.commit();
        }
        s.commit();
    }
    Store s;
    if (s.open(&fd, arena.data(), arena_pages) != RC::OK) return 1;
    u64 c0 = fd.st.read_calls, b0 = fd.st.read_bytes;
    double t0 = now_s();
    u64 n = 0, sum = 0;
    s.select(0, ~0ull, [&](u64, const void* p, u16 l) { ++n; sum += l + ((const u8*)p)[0]; return true; });
    double t1 = now_s();
    std::printf("value=%u B  arena=%d 页：扫 %llu 条  %.2f ms  pread=%llu  读 %.1f MB（%.0f MB/s）\n",
                vb, arena_pages, (unsigned long long)n, (t1 - t0) * 1e3,
                (unsigned long long)(fd.st.read_calls - c0), (fd.st.read_bytes - b0) / 1e6,
                (fd.st.read_bytes - b0) / 1e6 / (t1 - t0));
    (void)sum;
    return 0;
}

static int do_scan(int arena_pages, int nkeys, bool pf) {
    static std::vector<u8> arena(arena_pages * PAGE);
    const char* P = "build/bench5_scan.db"; std::remove(P);
    FileDev fd(P, 20000);
    {
        Store s;
        if (s.open(&fd, arena.data(), arena_pages) != RC::OK) return 1;
        std::vector<u8> v(60, 0x5A);
        for (int i = 0; i < nkeys; ++i) {
            s.put((u64)i * 3 + 1, v.data(), 60);
            if (i % 500 == 499) s.commit();
        }
        s.commit();
        s.checkpoint();
    }
    Store s;
    s.set_prefetch(pf);
    if (s.open(&fd, arena.data(), arena_pages) != RC::OK) return 1;
    u64 pread0 = fd.st.read_calls, rbytes0 = fd.st.read_bytes;
    double t0 = now_s();
    u64 n = 0, sum = 0;
    s.select(0, ~0ull, [&](u64, const void* p, u16 l) { ++n; sum += l + ((const u8*)p)[0]; return true; });
    double t1 = now_s();
    u64 preads = fd.st.read_calls - pread0, rb = fd.st.read_bytes - rbytes0;
    std::printf("prefetch=%s arena=%d 页  扫描 %llu 条（%.1f KB 值）  %.2f ms  pread=%llu  读 %llu KB\n",
                pf ? "on " : "off", arena_pages, (unsigned long long)n, (double)sum / 1e3, (t1 - t0) * 1e3,
                (unsigned long long)preads, (unsigned long long)(rb / 1024));
    return 0;
}

static int do_mt(int threads, int per_thread) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench5_mt.db"; std::remove(P);
    FileDev fd(P, 20000);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) return 1;
    std::atomic<int> errs{0};
    double t0 = now_s();
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) {
        th.emplace_back([&, t] {
            std::mt19937 rng(1000 + t);
            for (int i = 0; i < per_thread; ++i) {
                u64 k = (u64)t * 1000000 + rng() % 50000 + 1;
                u8 v[32];
                std::memcpy(v, &i, 4);
                std::memset(v + 4, (u8)k, 28);
                if (s.put(k, v, 32) != RC::OK) ++errs;
                if (i % 10 == 9 && s.commit() != RC::OK) ++errs;
            }
        });
    }
    for (auto& x : th) x.join();
    double t1 = now_s();
    if (s.commit() != RC::OK) ++errs;
    u32 merged = s.commits_merged();
    Report r;
    s.verify(&r);
    std::printf("%d 线程 × %d:  %.1f ms  错误=%d  commits_merged=%u  runs=%u  verify=%s\n",
                threads, per_thread, (t1 - t0) * 1e3, errs.load(), merged, s.run_count(),
                r.errors ? r.first_err : "OK");
    return errs || r.errors ? 1 : 0;
}

// 压实大值：接管链（默认）vs 物化重写。看 read_calls 与耗时。
static int do_consol(int take, int nkeys) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench5_consol.db"; std::remove(P);
    FileDev fd(P, 400000);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) return 1;
    s.set_ovfl_take(take != 0);
    std::vector<u8> v(8192, 0x5A);
    for (int i = 0; i < nkeys; ++i) {
        v[0] = (u8)i;
        s.put((u64)i + 1, v.data(), 8192);
        if (i % 250 == 249) s.commit();
    }
    s.commit();
    u32 runs0 = s.run_count();
    u64 c0 = fd.st.read_calls, w0 = fd.st.writes;
    double t0 = now_s();
    s.consolidate(0);
    double t1 = now_s();
    u64 reads = fd.st.read_calls - c0, writes = fd.st.writes - w0;
    Report r; s.verify(&r);
    std::printf("take=%d  runs=%u→%u  consolidate %.1f ms  读页=%llu 写页=%llu  verify=%s\n",
                take, runs0, s.run_count(), (t1 - t0) * 1e3,
                (unsigned long long)reads, (unsigned long long)writes,
                r.errors ? r.first_err : "OK");
    return r.errors ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "big";
    if (!std::strcmp(what, "bigscan")) return do_bigscan(argc > 2 ? atoi(argv[2]) : 128,
                                                          argc > 3 ? atoi(argv[3]) : 2000,
                                                          argc > 4 ? (u32)atoi(argv[4]) : 8192);
    if (!std::strcmp(what, "consol")) return do_consol(argc > 2 ? atoi(argv[2]) : 1, argc > 3 ? atoi(argv[3]) : 2000);
    if (!std::strcmp(what, "big")) return do_big(argc > 2 ? (u32)atoi(argv[2]) : 8192, argc > 3 ? atoi(argv[3]) : 2000);
    if (!std::strcmp(what, "scan")) return do_scan(argc > 2 ? atoi(argv[2]) : 256, argc > 3 ? atoi(argv[3]) : 20000, argc > 4 ? atoi(argv[4]) != 0 : true);
    if (!std::strcmp(what, "mt")) return do_mt(argc > 2 ? atoi(argv[2]) : 4, argc > 3 ? atoi(argv[3]) : 5000);
    std::printf("usage: bench5 big|scan|mt\n");
    return 2;
}
