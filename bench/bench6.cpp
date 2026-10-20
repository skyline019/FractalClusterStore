// bench6.cpp —— 第 7 轮：先补度量，再做优化。四个此前**没有数字**的维度：
//   ./bench6 snapmaint <nkeys> <rounds> [cf=0]   快照期维护（P5）：持快照反复覆盖写 + 维护，看高水位 / BUSY 数 / 回退正确性
//   ./bench6 rget   <threads> <nkeys> [arena_pages] [shared=1]   读并发：N 线程随机点查（全命中缓存 / 小缓存两种）
//   ./bench6 bigget <hot> <rounds> [vb]               大值重复读：hot 个 8 KB 值反复 get（链读有没有缓存）
//   ./bench6 delloop <rounds> <n>                     长删循环：del_range 半库 → 提交 → 回填 → 提交，看提交耗时/空间曲线
//   ./bench6 cfcurve <nkeys> <vb> <rounds>            大值库 compact_file 的空间曲线（覆盖 30% → compact_file）
//   ./bench6 fwrite <n> <commit_every> <vb> [async] [path]  FileDev 写路径：put n 条 vb 字节、每 commit_every 提交；
//                                                     打印 写调用数 / 写页数 / 写字节 / 用时（B4：写合并的度量）
//   ./bench6 selmem <n>                               读己之写的代价：n 条未提交，第一次 select / 不写重复 select /
//                                                     写查交错（每次 put 1 条再 select）/ 提交后 select（坑 77 的键序视图）
//   ./bench6 rngmem <n> <calls>                       del_range 在 n 条内存表上的单次成本（P4：键序视图 vs 逐条扫）
//   ./bench6 ovw <keys> <vb> <commits> <per> [every]      覆盖型负载（R0 悬崖的刻度）：keys 个键随机覆盖，每 per 条一次同步提交；
//                                                     每 every 次提交打印一行 runs / cells / 最大段数 / 层级直方图 / used / live，
//                                                     最后打印提交耗时分布、verify 与逐键回读（第 10 轮坑 88/89 的复现：20000 500 200 500）
//   ./bench6 bigput <n> <vb> [slow_us] [io_threads]                大值 put 延迟（P3 前链在前台同步写，P3 后交给 IO 线程）；slow_us>0 ⇒ 每次设备写睡 slow_us
//                                                     （慢设备上前台阻塞的量，P3 链写异步化的刻度）
// 所有数字都打印可复现的参数；bench 自己不做断言（verify 结果一并打印）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <cstdlib>
#include <algorithm>
using namespace fcs;

static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- rget
static int do_rget(int threads, int nkeys, int arena_pages, int shared) {
    static std::vector<u8> arena;
    arena.assign((size_t)arena_pages * PAGE, 0);
    const char* P = "build/bench6_rget.db"; std::remove(P);
    FileDev fd(P, 200000);
    Store s;
    if (s.open(&fd, arena.data(), arena_pages) != RC::OK) { std::printf("open fail\n"); return 1; }
    {
        std::vector<u8> v(60, 0x5A);
        for (int i = 0; i < nkeys; ++i) {
            std::memcpy(v.data(), &i, 4);
            s.put((u64)i + 1, v.data(), 60);
            if (i % 2000 == 1999) s.commit();
        }
        s.commit();
        s.consolidate(0);           // 一条 run：排除 run 数对读路径的影响
        s.checkpoint();
    }
    // 预热：把能进缓存的都进缓存（小缓存时自然进不去）
    {
        u8 out[64]; u16 l;
        for (int i = 0; i < nkeys; ++i) s.get((u64)i + 1, out, 64, &l);
    }
    s.set_shared_reads(shared != 0);           // P1：共享读模式（默认开，第 5 参=0 测独占老路）
    const int per = 200000 / threads;          // 总 20 万次
    std::atomic<u64> bad{0};
    u64 c0 = fd.st.read_calls;
    double t0 = now_s();
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) {
        th.emplace_back([&, t] {
            std::mt19937 rng(77 + t);
            u8 out[64]; u16 l = 0;
            for (int i = 0; i < per; ++i) {
                int k = (int)(rng() % (u32)nkeys);
                if (s.get((u64)k + 1, out, 64, &l) != RC::OK || l != 60) { ++bad; continue; }
                int kk; std::memcpy(&kk, out, 4);
                if (kk != k) ++bad;
            }
        });
    }
    for (auto& x : th) x.join();
    double t1 = now_s();
    const u64 n = (u64)per * threads;
    std::printf("rget threads=%d nkeys=%d arena=%d 页 shared=%d: %llu 次 get  %.1f ms  %.2f Mops/s  %.3f us/次  pread=%llu  bad=%llu\n",
                threads, nkeys, arena_pages, shared, (unsigned long long)n, (t1 - t0) * 1e3,
                n / (t1 - t0) / 1e6, (t1 - t0) * 1e6 / n,
                (unsigned long long)(fd.st.read_calls - c0), (unsigned long long)bad.load());
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- bigget
static int do_bigget(int hot, int rounds, u32 vb) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench6_bigget.db"; std::remove(P);
    FileDev fd(P, 400000);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    const int nkeys = 2000;
    std::vector<u8> v(vb, 0x5A);
    for (int i = 0; i < nkeys; ++i) {
        std::memcpy(v.data(), &i, 4);
        s.put((u64)i + 1, v.data(), (u16)vb);
        if (i % 250 == 249) s.commit();
    }
    s.commit();
    // 热键：均匀散在键空间里（不同 run）
    std::vector<int> hk(hot);
    for (int i = 0; i < hot; ++i) hk[i] = (int)((u64)i * nkeys / hot);
    std::vector<u8> out(vb);
    u64 bad = 0;
    u64 c0 = fd.st.read_calls, b0 = fd.st.read_bytes;
    double t0 = now_s();
    for (int r = 0; r < rounds; ++r) {
        int k = hk[r % hot];
        u16 l = 0;
        if (s.get((u64)k + 1, out.data(), (u16)vb, &l) != RC::OK || l != vb) { ++bad; continue; }
        int kk; std::memcpy(&kk, out.data(), 4);
        if (kk != k || out[vb - 1] != 0x5A) ++bad;
    }
    double t1 = now_s();
    std::printf("bigget hot=%d rounds=%d value=%u B: %.2f ms  %.2f us/次  pread=%llu  读 %.1f MB  bad=%llu\n",
                hot, rounds, vb, (t1 - t0) * 1e3, (t1 - t0) * 1e6 / rounds,
                (unsigned long long)(fd.st.read_calls - c0), (fd.st.read_bytes - b0) / 1e6,
                (unsigned long long)bad);
    // 对照：全部 2000 个各读一遍（无重复 ⇒ 缓存帮不上忙，看有没有额外开销）
    c0 = fd.st.read_calls; b0 = fd.st.read_bytes;
    t0 = now_s();
    for (int k = 0; k < nkeys; ++k) {
        u16 l = 0;
        if (s.get((u64)k + 1, out.data(), (u16)vb, &l) != RC::OK || l != vb) ++bad;
    }
    t1 = now_s();
    std::printf("bigget cold  all=%d  value=%u B: %.2f ms  %.2f us/次  pread=%llu  读 %.1f MB  bad=%llu\n",
                nkeys, vb, (t1 - t0) * 1e3, (t1 - t0) * 1e6 / nkeys,
                (unsigned long long)(fd.st.read_calls - c0), (fd.st.read_bytes - b0) / 1e6,
                (unsigned long long)bad);
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- delloop
static int do_delloop(int rounds, int n) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench6_delloop.db"; std::remove(P);
    FileDev fd(P, 400000);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::vector<u8> v(100, 0x33);
    auto fill = [&](u64 lo, u64 hi, u8 tag) {
        for (u64 k = lo; k < hi; ++k) {
            std::memcpy(v.data(), &k, 8); v[8] = tag;
            s.put(k, v.data(), 100);
            if ((k - lo) % 5000 == 4999) s.commit();
        }
        s.commit();
    };
    fill(1, (u64)n + 1, 0);
    std::printf("delloop n=%d 初始: used=%u live=%llu runs=%u cells=%u\n", n, s.used_pages(),
                (unsigned long long)s.live_page_count(), s.run_count(), s.cell_count());
    std::printf("%-5s %-9s %-9s %-9s %-7s %-7s %-7s %-5s %-5s %-6s %-7s %s\n",
                "round", "del_ms", "put_ms", "del_commit", "used", "live", "ratio", "tomb", "runs", "frees", "maxfree", "visible");
    int rc_all = 0;
    for (int r = 0; r < rounds; ++r) {
        // 删一半（交替删前半/后半，让两边的单元都经历“全覆盖”与“边界”两种情况）
        u64 lo = (r & 1) ? (u64)n / 2 + 1 : 1, hi = (r & 1) ? (u64)n + 1 : (u64)n / 2 + 1;
        double t0 = now_s();
        s.del_range(lo, hi);
        double t1 = now_s();
        RC rc = s.commit();
        double t2 = now_s();
        if (rc != RC::OK) { std::printf("commit fail round %d: %s\n", r, rc_str(rc)); rc_all = 1; break; }
        u32 vis = s.count(1, (u64)n + 1);
        if (vis != (u32)(n - (hi - lo))) { std::printf("  !! visible=%u expect=%llu\n", vis, (unsigned long long)(n - (hi - lo))); rc_all = 1; }
        double t3 = now_s();
        fill(lo, hi, (u8)(r + 1));
        double t4 = now_s();
        u32 used = s.used_pages(); u64 live = s.live_page_count();
        std::printf("%-5d %-9.2f %-9.1f %-10.2f %-7u %-7llu %-7.2f %-5u %-5u %-6u %-7u %u\n",
                    r, (t1 - t0) * 1e3, (t4 - t3) * 1e3, (t2 - t1) * 1e3, used, (unsigned long long)live,
                    live ? (double)used / live : 0.0, s.range_tombstones(), s.run_count(),
                    s.free_extents(), s.largest_free(), vis);
    }
    Report rep; s.verify(&rep);
    u32 vis = s.count(1, (u64)n + 1);
    std::printf("delloop 结束: used=%u live=%llu tomb=%u runs=%u visible=%u verify=%s\n", s.used_pages(),
                (unsigned long long)s.live_page_count(), s.range_tombstones(), s.run_count(), vis,
                rep.errors ? rep.first_err : "OK");
    if (vis != (u32)n) rc_all = 1;
    return rc_all || rep.errors ? 1 : 0;
}

// ---------------------------------------------------------------- cfcurve
static int do_cfcurve(int nkeys, u32 vb, int rounds) {
    static u8 arena[512 * PAGE];
    const char* P = "build/bench6_cf.db"; std::remove(P);
    FileDev fd(P, 1u << 20);
    Store s;
    if (s.open(&fd, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::vector<u8> v(vb, 0x77);
    std::mt19937 rng(99);
    auto putk = [&](u64 k, u8 tag) {
        std::memcpy(v.data(), &k, 8); v[8] = tag;
        s.put(k, v.data(), (u16)vb);
    };
    for (int i = 0; i < nkeys; ++i) { putk((u64)i + 1, 0); if (i % 500 == 499) s.commit(); }
    s.commit();
    std::printf("cfcurve nkeys=%d value=%u B 初始: used=%u live=%llu runs=%u\n", nkeys, vb, s.used_pages(),
                (unsigned long long)s.live_page_count(), s.run_count());
    std::printf("%-5s %-8s %-8s %-8s | %-9s %-8s %-8s %-8s %-8s %-6s %-5s %s\n",
                "round", "used0", "live0", "runs0", "cf_ms", "写页", "读次", "used1", "live1", "ratio", "runs", "verify");
    int bad = 0;
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < nkeys * 3 / 10; ++i) { putk((u64)(rng() % (u32)nkeys) + 1, (u8)(r + 1)); if (i % 500 == 499) s.commit(); }
        s.commit();
        u32 used0 = s.used_pages(); u64 live0 = s.live_page_count(); u32 runs0 = s.run_count();
        u64 w0 = fd.st.writes, rd0 = fd.st.read_calls;
        double t0 = now_s();
        RC rc = s.compact_file();
        double t1 = now_s();
        u64 wpg = fd.st.writes - w0, rpg = fd.st.read_calls - rd0;
        Report rep; s.verify(&rep);
        u32 used1 = s.used_pages(); u64 live1 = s.live_page_count();
        std::printf("%-5d %-8u %-8llu %-8u | %-9.1f %-8llu %-8llu %-8u %-8llu %-6.2f %-5u %s%s\n", r, used0,
                    (unsigned long long)live0, runs0, (t1 - t0) * 1e3, (unsigned long long)wpg, (unsigned long long)rpg,
                    used1, (unsigned long long)live1,
                    live1 ? (double)used1 / live1 : 0.0, s.run_count(),
                    rep.errors ? rep.first_err : "OK", rc != RC::OK ? " (compact_file 失败)" : "");
        if (rep.errors || rc != RC::OK) bad = 1;
    }
    // 回读校验
    std::vector<u8> out(vb);
    u64 nbad = 0;
    for (int i = 0; i < nkeys; ++i) {
        u16 l = 0;
        if (s.get((u64)i + 1, out.data(), (u16)vb, &l) != RC::OK || l != vb) { ++nbad; continue; }
        u64 k; std::memcpy(&k, out.data(), 8);
        if (k != (u64)i + 1) ++nbad;
    }
    std::printf("cfcurve 结束: 回读 bad=%llu  used=%u live=%llu\n", (unsigned long long)nbad, s.used_pages(),
                (unsigned long long)s.live_page_count());
    return bad || nbad ? 1 : 0;
}

// ---------------------------------------------------------------- fwrite
static int do_fwrite(int n, int every, u32 vb, int async_on, const char* path) {
    static u8 arena[1024 * PAGE];
    std::remove(path);
    FileDev fd(path, 600000);
    Store s;
    if (s.open(&fd, arena, 1024) != RC::OK) { std::printf("open fail\n"); return 1; }
    if (async_on) s.set_async_writes(true);
    std::vector<u8> v(vb, 0x5A);
    std::mt19937_64 rng(7);
    double t0 = now_s();
    for (int i = 0; i < n; ++i) {
        u64 k = rng() % ((u64)n * 4) + 1;
        std::memcpy(v.data(), &k, 8);
        s.put(k, v.data(), (u16)vb);
        if ((i + 1) % every == 0) s.commit();
    }
    s.commit();
    double t1 = now_s();
    Report rep; s.verify(&rep);
    std::printf("fwrite n=%d every=%d vb=%u async=%d: %.3f s | 写调用 %llu 写页 %llu 写字节 %.1f MB | used=%u runs=%u verify=%s\n",
                n, every, vb, async_on, t1 - t0,
                (unsigned long long)fd.st.write_calls, (unsigned long long)fd.st.writes,
                fd.st.write_bytes / 1048576.0, s.used_pages(), s.run_count(), rep.errors ? rep.first_err : "OK");
    std::remove(path);
    return rep.errors ? 1 : 0;
}

// ---------------------------------------------------------------- selmem（坑 77：select 归并内存表的代价）
static int do_selmem(u32 n) {
    static std::vector<u8> arena((size_t)4096 * PAGE);
    RamDev dev(1 << 18);
    Store s; s.open(&dev, arena.data(), 4096);
    u8 v[32] = {1};
    for (u32 i = 0; i < n; ++i) s.put(1 + (u64)i * 7919 % 1000003, v, 32);
    double t0 = now_s(); u32 c = s.count(1000, 1100); double t1 = now_s();
    const double first = (t1 - t0) * 1e3;
    t0 = now_s(); for (int i = 0; i < 1000; ++i) c += s.count(1000 + i, 1100 + i); t1 = now_s();
    const double cached = (t1 - t0) * 1e6 / 1000;
    t0 = now_s(); for (int i = 0; i < 200; ++i) { s.put(5 + i, v, 32); c += s.count(1000 + i, 1100 + i); } t1 = now_s();
    const double inter = (t1 - t0) * 1e6 / 200;
    s.commit();
    t0 = now_s(); for (int i = 0; i < 1000; ++i) c += s.count(1000 + i, 1100 + i); t1 = now_s();
    const double committed = (t1 - t0) * 1e6 / 1000;
    std::printf("selmem n=%u: 第一次 select %.2f ms（排一次）| 不写重复 %.2f µs/次 | 写查交错 %.1f µs/次 | 提交后 %.2f µs/次 (c=%u)\n",
                n, first, cached, inter, committed, c);
    return 0;
}

// ---------------------------------------------------------------- rngmem（P4：del_range 在大内存表上的成本）
static int do_rngmem(u32 n, u32 calls) {
    static std::vector<u8> arena((size_t)4096 * PAGE);
    RamDev dev(1 << 18);
    Store s; s.open(&dev, arena.data(), 4096);
    u8 v[32] = {1};
    for (u32 i = 0; i < n; ++i) s.put(1 + (u64)i * 7919 % 1000003, v, 32);
    double t0 = now_s();
    for (u32 i = 0; i < calls; ++i) s.del_range(2000000 + (u64)i * 10, 2000000 + (u64)i * 10 + 5);   // 不命中：纯查找成本
    double t1 = now_s();
    for (u32 i = 0; i < calls; ++i) s.del_range(1 + (u64)i * 7919 % 1000003, 2 + (u64)i * 7919 % 1000003);   // 各命中 1 条
    double t2 = now_s();
    std::printf("rngmem n=%u calls=%u: 不命中 %.2f µs/次 | 命中 1 条 %.2f µs/次 | 墓碑 %u\n",
                n, calls, (t1 - t0) * 1e6 / calls, (t2 - t1) * 1e6 / calls, s.range_tombstones());
    return 0;
}

// ---------------------------------------------------------------- bigput（P3 的刻度：大值 put 的同步链写）
namespace {
struct SlowFileDev : FileDev {
    u32 slow_us = 0;
    SlowFileDev(const char* p, PageId n, u32 us) : FileDev(p, n), slow_us(us) {}
    RC do_write(PageId pid, u32 off, const void* src, u32 n) override {
        if (slow_us) std::this_thread::sleep_for(std::chrono::microseconds(slow_us));
        return FileDev::do_write(pid, off, src, n);
    }
    RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) override {
        if (slow_us) std::this_thread::sleep_for(std::chrono::microseconds(slow_us));
        return FileDev::do_write_multi(pid, n, src, calls);
    }
};
}
static int do_bigput(u32 n, u32 vb, u32 slow_us, u32 io_threads) {
    static std::vector<u8> arena((size_t)4096 * PAGE);
    const char* P = "build/bench6_bigput.db"; std::remove(P);
    SlowFileDev fd(P, 1 << 17, slow_us);
    Store s; s.open(&fd, arena.data(), 4096);
    if (io_threads) s.set_io_threads(io_threads);
    s.set_async_writes(true);
    std::vector<u8> big(vb, 7), small(100, 3);
    double t0 = now_s();
    for (u32 i = 0; i < n; ++i) s.put(1 + i, small.data(), 100);
    double t1 = now_s();
    u64 c0 = fd.st.write_calls.load();
    for (u32 i = 0; i < n; ++i) s.put(1000000 + i, big.data(), (u16)vb);
    double t2 = now_s();
    u64 c1 = fd.st.write_calls.load();
    s.commit();
    double t3 = now_s();
    std::printf("bigput n=%u vb=%u slow_us=%u io_threads=%u (async on): 100 B put %.2f µs/次 | %u B put %.1f µs/次（前台写调用 %llu 次 = %.1f/put）| commit %.1f ms | put+commit 合计 %.1f ms\n",
                n, vb, slow_us, io_threads, (t1 - t0) * 1e6 / n, vb, (t2 - t1) * 1e6 / n,
                (unsigned long long)(c1 - c0), (double)(c1 - c0) / n, (t3 - t2) * 1e3, (t3 - t1) * 1e3);
    std::remove(P);
    return 0;
}

// ---------------------------------------------------------------- snapmaint（P5：快照期维护）
// 持一个快照不放，反复整库覆盖 + commit + consolidate/compact_file；看文件高水位涨到哪、维护被 BUSY 掉多少次，
// 最后 rollback 到快照逐键比对（放开维护之后回退点必须原样）。
// ---------------------------------------------------------------- ovw（R0）
static int do_ovw(u32 nkeys, u32 vb, u32 commits, u32 per, u32 every) {
    static u8 arena[1024 * PAGE];
    const char* P = "build/bench6_ovw.db"; std::remove(P);
    FileDev fd(P, 400000);
    Store s;
    if (s.open(&fd, arena, 1024) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::vector<u8> v(vb, 9);
    std::vector<u32> model(nkeys + 1, 0);         // 键 → 最后一次写入的序号（值前 4 字节）
    std::mt19937 rng(1);
    std::vector<double> ct; ct.reserve(commits);
    u32 seqno = 0;
    std::printf("ovw keys=%u vb=%u commits=%u per=%u\n", nkeys, vb, commits, per);
    std::printf("commit |  runs | cells | 最大段数 | 层级直方图(0..4) | used 页 | live 页 | 本区间提交 max ms\n");
    double t0 = now_s(), seg_max = 0;
    for (u32 c = 1; c <= commits; ++c) {
        for (u32 i = 0; i < per; ++i) {
            u64 k = rng() % nkeys + 1;
            ++seqno;
            std::memcpy(v.data(), &seqno, 4);
            if (s.put(k, v.data(), vb) != RC::OK) { std::printf("put fail\n"); return 1; }
            model[k] = seqno;
        }
        double a = now_s();
        if (s.commit() != RC::OK) { std::printf("commit fail\n"); return 1; }
        double d = (now_s() - a) * 1e3;
        ct.push_back(d); seg_max = std::max(seg_max, d);
        if (every && c % every == 0) {
            auto h = s.lvl_hist();
            std::printf("%6u | %5u | %5u | %8u | %u %u %u %u %u | %7u | %7llu | %.1f\n", c, s.run_count(), s.cell_count(),
                        s.max_segs_per_cell(), h[0], h[1], h[2], h[3], h[4], s.used_pages(), (unsigned long long)s.live_page_count(), seg_max);
            seg_max = 0;
        }
    }
    double wall = now_s() - t0;
    std::vector<double> so = ct; std::sort(so.begin(), so.end());
    double sum = 0; for (double x : ct) sum += x;
    std::printf("提交 %u 次：avg %.1f ms  p50 %.1f  p90 %.1f  p99 %.1f  max %.1f ms  (>50 ms: %zu)  墙钟 %.2f s\n", commits, sum / commits,
                so[commits / 2], so[commits * 9 / 10], so[std::min<size_t>(commits - 1, commits * 99 / 100)], so.back(),
                (size_t)std::count_if(ct.begin(), ct.end(), [](double x) { return x > 50; }), wall);
    // 逐键回读：值前 4 字节 = 该键最后一次写入的序号（坑 89 的改动动了压实产物的层号，这里直接查\"旧版本有没有压过新版本\"）
    u32 bad = 0;
    std::vector<u8> buf(vb + 16);
    for (u64 k = 1; k <= nkeys; ++k) {
        u16 len = 0;
        RC rc = s.get(k, buf.data(), (u16)buf.size(), &len);
        if (!model[k]) { if (rc == RC::OK) ++bad; continue; }
        u32 got = 0; if (rc == RC::OK && len == vb) std::memcpy(&got, buf.data(), 4);
        if (rc != RC::OK || len != vb || got != model[k]) ++bad;
    }
    Report r; s.verify(&r);
    auto h = s.lvl_hist();
    std::printf("末态：runs=%u cells=%u 最大段数=%u 层级 %u %u %u %u %u  used=%u live=%llu (%.2fx)  回读 bad=%u  verify=%s\n",
                s.run_count(), s.cell_count(), s.max_segs_per_cell(), h[0], h[1], h[2], h[3], h[4], s.used_pages(),
                (unsigned long long)s.live_page_count(), s.live_page_count() ? (double)s.used_pages() / (double)s.live_page_count() : 0.0,
                bad, r.errors ? r.first_err : "OK");
    {   // 写放大：设备实际写字节 / 逻辑写字节（每条记录 vb + 8 字节键 + 头），老新引擎对照用
        double logical = (double)commits * per * (vb + 8.0 + 10.0);
        std::printf("设备写 %.1f MB，逻辑写 %.1f MB，写放大 %.2fx\n", fd.st.write_bytes.load() / 1048576.0, logical / 1048576.0,
                    fd.st.write_bytes.load() / logical);
    }
    std::remove(P);
    return (bad || r.errors) ? 1 : 0;
}

static int do_snapmaint(u32 nkeys, u32 rounds, int use_cf) {
    static std::vector<u8> arena((size_t)1024 * PAGE);
    RamDev dev(1 << 16);
    Store s; if (s.open(&dev, arena.data(), 1024) != RC::OK) { std::printf("open fail\n"); return 1; }
    std::vector<u8> v(60, 0);
    auto fill = [&](u32 k, u32 r) { std::memset(v.data(), (int)(r & 0xFF), 60); std::memcpy(v.data(), &k, 4); std::memcpy(v.data() + 4, &r, 4); };
    for (u32 i = 0; i < nkeys; ++i) { fill(i, 0); s.put(1 + i, v.data(), 60); }
    s.commit(); s.consolidate(0);
    const u32 used0 = s.used_pages();
    if (s.snapshot() != RC::OK) { std::printf("snapshot fail\n"); return 1; }
    u32 busy = 0, okc = 0, peak = used0;
    double t0 = now_s();
    for (u32 r = 1; r <= rounds; ++r) {
        for (u32 i = 0; i < nkeys; ++i) { fill(i, r); s.put(1 + i, v.data(), 60); }
        s.commit();
        RC rc = use_cf ? s.compact_file() : s.consolidate(0);
        if (rc == RC::BUSY) ++busy; else if (rc == RC::OK) ++okc;
        peak = std::max(peak, s.used_pages());
    }
    double t1 = now_s();
    const u32 used1 = s.used_pages();
    // 回退点必须原样
    u64 bad = 0;
    if (s.rollback(0) != RC::OK) { std::printf("rollback fail\n"); return 1; }
    for (u32 i = 0; i < nkeys; ++i) {
        u8 out[64]; u16 l = 0;
        if (s.get(1 + i, out, 64, &l) != RC::OK || l != 60) { ++bad; continue; }
        u32 k, r; std::memcpy(&k, out, 4); std::memcpy(&r, out + 4, 4);
        if (k != i || r != 0) ++bad;
    }
    const bool vok = s.verify() == RC::OK;
    std::printf("snapmaint nkeys=%u rounds=%u %s: 快照前 used=%u 页 → 快照期峰值 %u、结束 %u 页（%.2fx）| 维护 ok=%u busy=%u | %.1f ms | 回退后错 %llu verify=%s\n",
                nkeys, rounds, use_cf ? "compact_file" : "consolidate", used0, peak, used1, (double)peak / used0, okc, busy,
                (t1 - t0) * 1e3, (unsigned long long)bad, vok ? "ok" : "FAIL");
    return (bad || !vok) ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "";
    if (!std::strcmp(what, "ovw")) return do_ovw(argc > 2 ? (u32)atoi(argv[2]) : 20000, argc > 3 ? (u32)atoi(argv[3]) : 500, argc > 4 ? (u32)atoi(argv[4]) : 200, argc > 5 ? (u32)atoi(argv[5]) : 500, argc > 6 ? (u32)atoi(argv[6]) : 50);
    if (!std::strcmp(what, "snapmaint")) return do_snapmaint(argc > 2 ? (u32)atoi(argv[2]) : 20000, argc > 3 ? (u32)atoi(argv[3]) : 20, argc > 4 ? atoi(argv[4]) : 0);
    if (!std::strcmp(what, "selmem")) return do_selmem(argc > 2 ? (u32)atoi(argv[2]) : 100000);
    if (!std::strcmp(what, "rngmem")) return do_rngmem(argc > 2 ? (u32)atoi(argv[2]) : 100000, argc > 3 ? (u32)atoi(argv[3]) : 1000);
    if (!std::strcmp(what, "bigput")) return do_bigput(argc > 2 ? (u32)atoi(argv[2]) : 2000, argc > 3 ? (u32)atoi(argv[3]) : 8192, argc > 4 ? (u32)atoi(argv[4]) : 0, argc > 5 ? (u32)atoi(argv[5]) : 0);
    if (!std::strcmp(what, "fwrite"))
        return do_fwrite(argc > 2 ? atoi(argv[2]) : 200000, argc > 3 ? atoi(argv[3]) : 2000,
                         argc > 4 ? (u32)atoi(argv[4]) : 100, argc > 5 ? atoi(argv[5]) : 0,
                         argc > 6 ? argv[6] : "build/bench6_fwrite.db");
    if (!std::strcmp(what, "rget"))
        return do_rget(argc > 2 ? atoi(argv[2]) : 1, argc > 3 ? atoi(argv[3]) : 100000, argc > 4 ? atoi(argv[4]) : 4096,
                       argc > 5 ? atoi(argv[5]) : 1);
    if (!std::strcmp(what, "bigget"))
        return do_bigget(argc > 2 ? atoi(argv[2]) : 16, argc > 3 ? atoi(argv[3]) : 20000, argc > 4 ? (u32)atoi(argv[4]) : 8192);
    if (!std::strcmp(what, "delloop"))
        return do_delloop(argc > 2 ? atoi(argv[2]) : 6, argc > 3 ? atoi(argv[3]) : 100000);
    if (!std::strcmp(what, "cfcurve"))
        return do_cfcurve(argc > 2 ? atoi(argv[2]) : 20000, argc > 3 ? (u32)atoi(argv[3]) : 3900, argc > 4 ? atoi(argv[4]) : 4);
    std::printf("usage: bench6 ovw <keys> <vb> <commits> <per> [every] | snapmaint <nkeys> <rounds> [cf] | rget <threads> <nkeys> [arena] [shared] | bigget <hot> <rounds> [vb] | delloop <rounds> <n> | cfcurve <nkeys> <vb> <rounds> | fwrite <n> <every> <vb> [async] [path] | selmem <n> | rngmem <n> <calls> | bigput <n> <vb> [slow_us] [io_threads]\n");
    return 2;
}
