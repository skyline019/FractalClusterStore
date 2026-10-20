// bench4.cpp — FCS vs SQLite 同负载正面对比
//
// 两个引擎跑完全相同的四段负载（同一批键、同样的提交批次、同样的读序列）：
//   P1 写入：N 个随机键，每批 B 个提交一次（= 一次持久化屏障）
//   P2 更新：N/2 次覆盖写，同样每批 B 提交
//   P3 点读：R 次随机点查
//   P4 范围：S 次区间扫描，每次 K 条，顺序遍历
//
// 计量：µs/op、写入字节（引擎自计 + /proc/self/io 实测）、空间放大、读放大。
// SQLite 配置：WAL + synchronous=FULL（每次 COMMIT 落盘，与 FCS 的提交屏障对等）。
#include "fcs.h"

#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>

using namespace xdb;
using namespace fcs;
using u64 = uint64_t;
using u32 = uint32_t;

static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}
static double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(steady_clock::now().time_since_epoch()).count();
}
// /proc/self/io 的 write_bytes（真正到达块层的字节）
static u64 io_wbytes() {
    FILE* f = std::fopen("/proc/self/io", "r");
    if (!f) return 0;
    char line[128]; u64 v = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "write_bytes:", 12) == 0) v = std::strtoull(line + 12, nullptr, 10);
    std::fclose(f);
    return v;
}
static u64 file_bytes(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    u64 n = (u64)std::ftell(f);
    std::fclose(f);
    return n;
}

struct Row { const char* name; double us; double ops; u64 wbytes; u64 logical; };

int main(int argc, char** argv) {
    u32 N   = argc > 1 ? (u32)atoi(argv[1]) : 50000;   // 键数
    u32 B   = argc > 2 ? (u32)atoi(argv[2]) : 500;     // 每批提交的操作数
    u32 VSZ = argc > 3 ? (u32)atoi(argv[3]) : 32;      // 值长度
    u32 R   = argc > 4 ? (u32)atoi(argv[4]) : 50000;   // 点读次数
    u32 S   = argc > 5 ? (u32)atoi(argv[5]) : 2000;    // 区间扫描次数
    u32 K   = argc > 6 ? (u32)atoi(argv[6]) : 100;     // 每次区间条数
    int kidx = argc > 7 ? atoi(argv[7]) : 0;           // 1 = 打开精确键索引
    int async = argc > 8 ? atoi(argv[8]) : 0;          // 1 = 异步页写
    int gm    = argc > 9 ? atoi(argv[9]) : 0;          // 1 = 组提交（批内 commit(false)）
    int lt    = argc > 10 ? atoi(argv[10]) : 0;        // 每层 run 上限（0 = 默认 8）
    int am    = argc > 11 ? atoi(argv[11]) : 0;        // async 一次落多少条（0 = 默认 2048）
    int lvl_t = lt ? lt : 8;
    int cons = argc > 12 ? atoi(argv[12]) : 0;    // 0=不归一 1=全库归一 N>=2=只归一“段数≥N”的单元
    int shr  = argc > 13 ? atoi(argv[13]) : 0;    // 1 = 归一再把文件收缩回去
    int ithr = argc > 14 ? atoi(argv[14]) : 1;    // 异步写线程数
    int kmode = argc > 15 ? atoi(argv[15]) : 0;   // 0=均匀随机键 1=时间型有序键
    int hwpct = argc > 16 ? atoi(argv[16]) : 0;   // 第 14 轮：高水位回收阈值（百分比，0 = 关）

    const u64 KEYMASK = SKEY_MAX;                      // 56 位键（SQLite INTEGER 为有符号）
    std::vector<u64> keys;
    keys.reserve(N);
    for (u32 i = 0; i < N; ++i) {
        if (kmode) keys.push_back(1700000000000ull + (u64)i * 13 + (mix(i) & 7));   // 时间戳型
        else keys.push_back(mix(i * 2654435761u + 12345) & KEYMASK);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    N = (u32)keys.size();

    std::vector<u8> val(VSZ), val2(VSZ);
    for (u32 i = 0; i < VSZ; ++i) { val[i] = (u8)(i * 31 + 7); val2[i] = (u8)(i * 17 + 3); }

    std::printf("# FCS vs SQLite   N=%u B=%u val=%uB 读=%u 区间=%u×%u  kidx=%d async=%d group=%d lvl_t=%d key=%s\n",
                N, B, VSZ, R, S, K, kidx, async, gm, lvl_t, kmode ? "时间型" : "随机");
    std::printf("# 单位: µs/op（越小越好） | 写字节 = 引擎自计 / 块层实测 / 逻辑值字节\n");

    // ---------------------------------------------------------------- FCS
    const char* fpath = "/home/user/.fcs_bench.db";
    std::remove(fpath);
    Row f1r{"fcs insert", 0, 1, 0, 0}, f2r{"fcs update", 0, 1, 0, 0}, f3r{"fcs get", 0, 1, 0, 0}, f4r{"fcs range", 0, 1, 0, 0};
    u64 fcs_live = 0, fcs_space = 0, fcs_rbytes = 0;
    {
        FileDev dev(fpath, (PageId)((N * (VSZ + 24) + 8 * 1024 * 1024) / PAGE + 4096));
        if (!dev.ok()) { std::printf("fcs file open failed\n"); return 1; }
        Store s;
        if (s.open(&dev, nullptr, 2048) != RC::OK) { std::printf("fcs open failed\n"); return 1; }
        s.set_key_index(kidx != 0);
        s.set_async_writes(async != 0);
        if (lt) s.set_lvl_t((u32)lt);
        if (hwpct) s.set_hw_reclaim_pct((u32)hwpct);
        if (am) s.set_async_flush_min((u32)am);
        s.set_io_threads((u32)(ithr > 0 ? ithr : 1));
        auto cb = [&] { if (gm) s.commit(false); else s.commit(); };

            u64 w0 = io_wbytes(), e0 = dev.st.write_bytes;
        double t0 = now_us(), cmax = 0;
        for (u32 i = 0; i < N; ++i) {
            if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) { std::printf("put fail %u\n", i); return 1; }
            if ((i + 1) % B == 0) { double c0 = now_us(); cb(); double d = now_us() - c0; if (d > cmax) cmax = d; }
        }
        { double c0 = now_us(); s.commit(); double d = now_us() - c0; if (d > cmax) cmax = d; }
        std::printf("# fcs insert: max commit = %.0f µs  mem high-water = %llu B\n", cmax, (unsigned long long)s.mem_high_water());
        f1r.us = (now_us() - t0) / N; f1r.ops = N;
        f1r.wbytes = io_wbytes() - w0; f1r.logical = dev.st.write_bytes - e0;

        u64 e1 = dev.st.write_bytes; t0 = now_us();
        for (u32 i = 0; i < N / 2; ++i) {
            u64 k = keys[mix(i + 999) % N];
            s.put(k, val2.data(), (u16)VSZ);
            if ((i + 1) % B == 0) cb();
        }
        s.commit();
        f2r.us = (now_us() - t0) / (N / 2); f2r.ops = N / 2;
        f2r.logical = dev.st.write_bytes - e1;

        if (cons) {
            double c0 = now_us();
            RC crc = (cons == 1 && shr) ? s.compact_file() : s.consolidate(cons == 1 ? 0 : (u32)cons);
            double el = (now_us() - c0) / 1000.0;
            if (shr) {
                double c1 = now_us();
                RC src = (cons == 1) ? crc : s.shrink();
                el += (now_us() - c1) / 1000.0;
                std::printf("# fcs 收缩: rc=%d  文件 %llu B (%llu 页)  top=%u 活页=%u 空闲块=%u/最大%u  合计 %.1f ms\n",
                    (int)src, (unsigned long long)dev.file_bytes(),
                    (unsigned long long)dev.capacity(), s.live_top(), s.live_pages(),
                    s.free_extents(), s.largest_free(), el);
            } else {
                std::printf("# fcs consolidate: rc=%d  %.1f ms\n", (int)crc, el);
            }
        }
        u64 r0 = dev.st.read_bytes, g0 = s.gets(), p0 = s.probes();
        t0 = now_us();
        u32 hits = 0;
        for (u32 i = 0; i < R; ++i) {
            u8 buf[64]; u16 l = 0;
            if (s.get(keys[mix(i * 7 + 1) % N], buf, sizeof buf, &l) == RC::OK) ++hits;
        }
        f3r.us = (now_us() - t0) / R; f3r.ops = R;
        fcs_rbytes = dev.st.read_bytes - r0;
        std::printf("# fcs reads: %.2f 页/get  probes/get=%.2f  hits=%u\n",
                    R ? (double)(dev.st.read_bytes - r0) / PAGE / R : 0.0,
                    R ? (double)(s.probes() - p0) / R : 0.0, hits);

        u64 ch0 = s.cache_hits(), cm0 = s.cache_misses(), sk0 = s.sp_skips(), co0 = s.cursors_opened();
        t0 = now_us();
        u64 got = 0;
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % N];
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++got; return true; }, K);
        }
        f4r.us = (now_us() - t0) / (double)S; f4r.ops = S;
        std::printf("# fcs range cache: hits/scan=%.2f misses/scan=%.3f sp_skips/scan=%.2f\n",
                    (double)(s.cache_hits() - ch0) / S, (double)(s.cache_misses() - cm0) / S,
                    (double)(s.sp_skips() - sk0) / S);
        std::printf("# fcs io jobs=%llu  async=%d\n", (unsigned long long)s.io_done(), async);
    std::printf("# fcs range cursors/scan=%.2f\n", (double)(s.cursors_opened() - co0) / S);
        std::printf("# fcs range: %.2f µs/scan  rows=%llu  (%.2f µs/row)\n",
                    f4r.us, (unsigned long long)got, got ? f4r.us * S / (double)got : 0.0);

        s.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++fcs_live; return true; });
        { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 30e6) s.commit(); s.commit(); }   // 第 14 轮：把欠的维护干完、最后一批释放页过提交点，再量空间
        Report rep;
        s.verify(&rep);
        fcs_space = (u64)s.used_pages() * PAGE;      // 实际在用空间（文件是预分配的）
        std::printf("# fcs space: file=%llu B hw=%llu B (live_pages=%llu free=%llu largest_free=%u pg extents=%u)  live_recs=%llu  bytes/rec=%.1f  runs=%u cells=%u  hw_reclaims=%llu pending=%d\n",
                    (unsigned long long)dev.file_bytes(), (unsigned long long)fcs_space,
                    (unsigned long long)((u64)s.live_pages() * PAGE),
                    (unsigned long long)(s.free_page_count() * PAGE), s.largest_free(), s.free_extents(),
                    (unsigned long long)fcs_live,
                    fcs_live ? (double)fcs_space / fcs_live : 0.0, rep.runs, rep.cells, (unsigned long long)s.maint_stats().hw_reclaims, (int)s.maint_pending());
    }

    // -------------------------------------------------------------- SQLite
    const char* spath = "/home/user/.sqlite_bench.db";
    std::remove(spath);
    std::remove((std::string(spath) + "-wal").c_str());
    std::remove((std::string(spath) + "-shm").c_str());
    Row s1r{"sqlite insert", 0, 1, 0, 0}, s2r{"sqlite update", 0, 1, 0, 0}, s3r{"sqlite get", 0, 1, 0, 0}, s4r{"sqlite range", 0, 1, 0, 0};
    u64 sql_live = 0, sql_space = 0;
    {
        sqlite3* db = nullptr;
        if (sqlite3_open(spath, &db) != SQLITE_OK) { std::printf("sqlite open failed\n"); return 1; }
        char* err = nullptr;
        auto ex = [&](const char* sql) {
            if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::printf("sqlite exec failed: %s (%s)\n", sql, err ? err : "");
                std::exit(1);
            }
        };
        ex("PRAGMA page_size=4096");
        ex("PRAGMA journal_mode=WAL");
        ex("PRAGMA synchronous=FULL");
        ex("PRAGMA cache_size=-16384");            // 16 MB 页缓存，与 FCS 的 arena 同级
        ex("CREATE TABLE kv(k INTEGER PRIMARY KEY, v BLOB)");

        sqlite3_stmt* ins = nullptr; sqlite3_stmt* get = nullptr; sqlite3_stmt* rng = nullptr;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv VALUES(?,?)", -1, &ins, nullptr);
        sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k=?", -1, &get, nullptr);
        sqlite3_prepare_v2(db, "SELECT k,v FROM kv WHERE k>=? AND k<? ORDER BY k LIMIT ?", -1, &rng, nullptr);

        u64 w0 = io_wbytes();
        double t0 = now_us();
        for (u32 i = 0; i < N; ++i) {
            if (i % B == 0) ex("BEGIN");
            sqlite3_bind_int64(ins, 1, (sqlite3_int64)keys[i]);
            sqlite3_bind_blob(ins, 2, val.data(), (int)VSZ, SQLITE_STATIC);
            sqlite3_step(ins); sqlite3_reset(ins);
            if ((i + 1) % B == 0) ex("COMMIT");
        }
        if (N % B) ex("COMMIT");
        s1r.us = (now_us() - t0) / N; s1r.ops = N;
        s1r.wbytes = io_wbytes() - w0;

        t0 = now_us();
        for (u32 i = 0; i < N / 2; ++i) {
            if (i % B == 0) ex("BEGIN");
            u64 k = keys[mix(i + 999) % N];
            sqlite3_bind_int64(ins, 1, (sqlite3_int64)k);
            sqlite3_bind_blob(ins, 2, val2.data(), (int)VSZ, SQLITE_STATIC);
            sqlite3_step(ins); sqlite3_reset(ins);
            if ((i + 1) % B == 0) ex("COMMIT");
        }
        if ((N / 2) % B) ex("COMMIT");
        s2r.us = (now_us() - t0) / (N / 2); s2r.ops = N / 2;

        t0 = now_us();
        for (u32 i = 0; i < R; ++i) {
            sqlite3_bind_int64(get, 1, (sqlite3_int64)keys[mix(i * 7 + 1) % N]);
            if (sqlite3_step(get) == SQLITE_ROW) { sqlite3_column_blob(get, 0); }
            sqlite3_reset(get);
        }
        s3r.us = (now_us() - t0) / R; s3r.ops = R;

        t0 = now_us();
        u64 got = 0;
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % N];
            sqlite3_bind_int64(rng, 1, (sqlite3_int64)lo);
            sqlite3_bind_int64(rng, 2, (sqlite3_int64)(lo + 4096));
            sqlite3_bind_int(rng, 3, (int)K);
            while (sqlite3_step(rng) == SQLITE_ROW) {
                sqlite3_column_int64(rng, 0); sqlite3_column_blob(rng, 1); ++got;
            }
            sqlite3_reset(rng);
        }
        s4r.us = (now_us() - t0) / (double)S; s4r.ops = S;
        std::printf("# sqlite range: %.2f µs/scan rows=%llu (%.2f µs/row)\n",
                    s4r.us, (unsigned long long)got, got ? s4r.us * S / (double)got : 0.0);

        sqlite3_finalize(ins); sqlite3_finalize(get); sqlite3_finalize(rng);
        // 计数
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM kv", -1, &c, nullptr) == SQLITE_OK) {
            if (sqlite3_step(c) == SQLITE_ROW) sql_live = (u64)sqlite3_column_int64(c, 0);
            sqlite3_finalize(c);
        }
        ex("PRAGMA wal_checkpoint(TRUNCATE)");
        sqlite3_close(db);
        sql_space = file_bytes(spath);
        std::printf("# sqlite space: file=%llu B live_recs=%llu bytes/rec=%.1f\n",
                    (unsigned long long)sql_space, (unsigned long long)sql_live,
                    sql_live ? (double)sql_space / sql_live : 0.0);
    }

    // ---------------------------------------------------------------- 对照
    std::printf("\n%-14s %12s %16s %16s\n", "phase", "us/op", "block B/op", "logical B/op");
    Row* rows[] = {&f1r, &s1r, &f2r, &s2r, &f3r, &s3r, &f4r, &s4r};
    for (Row* r : rows)
        std::printf("%-14s %12.3f %16.1f %16.1f\n", r->name, r->us,
                    r->ops ? (double)r->wbytes / r->ops : 0.0,
                    r->ops ? (double)r->logical / r->ops : 0.0);
    std::printf("\n%-14s %12s %12s %12s\n", "", "fcs", "sqlite", "fcs/sqlite");
    std::printf("%-14s %12.3f %12.3f %12.2f\n", "insert us/op", f1r.us, s1r.us, s1r.us ? f1r.us / s1r.us : 0.0);
    std::printf("%-14s %12.3f %12.3f %12.2f\n", "update us/op", f2r.us, s2r.us, s2r.us ? f2r.us / s2r.us : 0.0);
    std::printf("%-14s %12.3f %12.3f %12.2f\n", "get us/op", f3r.us, s3r.us, s3r.us ? f3r.us / s3r.us : 0.0);
    std::printf("%-14s %12.3f %12.3f %12.2f\n", "range us/scan", f4r.us, s4r.us, s4r.us ? f4r.us / s4r.us : 0.0);
    std::printf("%-14s %12llu %12llu\n", "space bytes", (unsigned long long)fcs_space, (unsigned long long)sql_space);
    std::printf("%-14s %12llu %12llu\n", "block write B", (unsigned long long)f1r.wbytes, (unsigned long long)s1r.wbytes);
    return 0;
}
