// bench7.cpp — FCS vs LMDB 真机对比（LittleFS 暂缓）
//
// 公平原则：只比双方都擅长的有序 KV 能力。
//   LittleFS 是闪存文件系统，非有序 KV —— 暂不纳入（见 PEERS.md）；
//   编译加 -DBENCH7_WITH_LFS 可重新打开（仍禁测 LFS.range）。
//
// 负载：
//   P1 写入 N 键，每批 B 持久屏障
//   P2 覆盖 N/2
//   P3 点读 R
//   P4 区间 S×K（FCS/LMDB 均有游标）
//
// 用法：build/bench7 [N] [B] [VSZ] [R] [S] [K] [async] [gm] [cons] [shr]
#include "fcs.h"

#include <lmdb.h>
#ifdef BENCH7_WITH_LFS
extern "C" {
#include "lfs.h"
}
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

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
static u64 file_bytes(const char* path) {
    struct stat st {};
    if (stat(path, &st) != 0) return 0;
    return (u64)st.st_size;
}
static void key_be(u64 k, u8 out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = (u8)(k >> (56 - 8 * i));
}
static u64 be_key(const void* p) {
    const u8* b = (const u8*)p;
    u64 k = 0;
    for (int i = 0; i < 8; ++i) k = (k << 8) | b[i];
    return k;
}

struct Row { const char* name; double us; u64 space; };

int main(int argc, char** argv) {
    u32 N   = argc > 1 ? (u32)atoi(argv[1]) : 50000;
    u32 B   = argc > 2 ? (u32)atoi(argv[2]) : 500;
    u32 VSZ = argc > 3 ? (u32)atoi(argv[3]) : 32;
    u32 R   = argc > 4 ? (u32)atoi(argv[4]) : 50000;
    u32 S   = argc > 5 ? (u32)atoi(argv[5]) : 2000;
    u32 K   = argc > 6 ? (u32)atoi(argv[6]) : 100;
    int async = argc > 7 ? atoi(argv[7]) : 1;
    int gm    = argc > 8 ? atoi(argv[8]) : 1;
    int cons  = argc > 9 ? atoi(argv[9]) : 1;
    int shr   = argc > 10 ? atoi(argv[10]) : 1;
    int soft  = argc > 11 ? atoi(argv[11]) : 0;   // 1 = Soft / configure_deploy

    const u64 KEYMASK = SKEY_MAX;
    std::vector<u64> keys;
    keys.reserve(N);
    for (u32 i = 0; i < N; ++i)
        keys.push_back(mix(i * 2654435761u + 12345) & KEYMASK);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    N = (u32)keys.size();

    std::vector<u8> val(VSZ), val2(VSZ);
    for (u32 i = 0; i < VSZ; ++i) { val[i] = (u8)(i * 31 + 7); val2[i] = (u8)(i * 17 + 3); }

    std::printf("# FCS vs LMDB  N=%u B=%u val=%uB R=%u S=%u×%u  fcs(async=%d group=%d cons=%d shr=%d soft=%d)\n",
                N, B, VSZ, R, S, K, async, gm, cons, shr, soft);
    std::printf("# LittleFS: 暂缓（非有序KV；禁测其不擅长能力）。每 B 次一持久屏障 | LMDB 大端键\n");
    std::fflush(stdout);

    Row fi{}, fu{}, fg{}, fr{};
    Row li{}, lu{}, lg{}, lr{};

    // ============================================================ FCS
    {
        const char* fpath = "/home/user/.fcs_peers.db";
        std::remove(fpath);
        FileDev dev(fpath, (PageId)((N * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
        if (!dev.ok()) { std::printf("fcs open file failed\n"); return 1; }
        Store s;
        if (s.open(&dev, nullptr, 2048) != RC::OK) { std::printf("fcs open failed\n"); return 1; }
        if (soft) s.set_tidy_policy(Store::TidyPolicy::Soft);
        else {
            s.set_async_writes(async != 0);
            s.set_io_threads(1);
        }
        auto cb = [&] { if (gm) s.commit(false); else s.commit(); };

        double t0 = now_us();
        for (u32 i = 0; i < N; ++i) {
            if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) { std::printf("fcs put fail\n"); return 1; }
            if ((i + 1) % B == 0) cb();
        }
        s.commit();
        fi.us = (now_us() - t0) / N;

        t0 = now_us();
        for (u32 i = 0; i < N / 2; ++i) {
            s.put(keys[mix(i + 999) % N], val2.data(), (u16)VSZ);
            if ((i + 1) % B == 0) cb();
        }
        s.commit();
        fu.us = (now_us() - t0) / (N / 2);

        if (cons) {
            RC crc = (cons == 1 && shr) ? s.compact_file() : s.consolidate(0);
            if (shr && cons != 1) s.shrink();
            (void)crc;
        }

        t0 = now_us();
        for (u32 i = 0; i < R; ++i) {
            u8 buf[64]; u16 l = 0;
            s.get(keys[mix(i * 7 + 1) % N], buf, sizeof buf, &l);
        }
        fg.us = (now_us() - t0) / R;

        t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % N], n = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++n; return true; }, K);
        }
        fr.us = (now_us() - t0) / (double)S;

        { double w0 = now_us(); while (s.maint_pending() && now_us() - w0 < 30e6) s.commit(); s.commit(); }
        fi.space = (u64)s.used_pages() * PAGE;
        std::printf("# fcs done insert=%.3f update=%.3f get=%.3f range=%.3f space=%llu B\n",
                    fi.us, fu.us, fg.us, fr.us, (unsigned long long)fi.space);
        std::fflush(stdout);
    }

    // ============================================================ LMDB
    {
        const char* dpath = "/home/user/.lmdb_peers";
        char cmd[256];
        std::snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dpath, dpath);
        if (std::system(cmd) != 0) { std::printf("lmdb mkdir failed\n"); return 1; }

        MDB_env* env = nullptr;
        if (mdb_env_create(&env) != 0) { std::printf("mdb_env_create\n"); return 1; }
        mdb_env_set_mapsize(env, (size_t)N * (VSZ + 64) + 64ull * 1024 * 1024);
        mdb_env_set_maxdbs(env, 1);
        if (mdb_env_open(env, dpath, 0, 0664) != 0) { std::printf("mdb_env_open\n"); return 1; }

        MDB_txn* txn = nullptr;
        MDB_dbi dbi;
        if (mdb_txn_begin(env, nullptr, 0, &txn) != 0) return 1;
        if (mdb_dbi_open(txn, nullptr, 0, &dbi) != 0) return 1;
        mdb_txn_commit(txn);

        auto put_batch = [&](bool upd) {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, 0, &t);
            u32 count = upd ? N / 2 : N;
            for (u32 i = 0; i < count; ++i) {
                if (i && i % B == 0) {
                    mdb_txn_commit(t);
                    mdb_txn_begin(env, nullptr, 0, &t);
                }
                u64 k = upd ? keys[mix(i + 999) % N] : keys[i];
                const u8* v = upd ? val2.data() : val.data();
                u8 kb[8]; key_be(k, kb);
                MDB_val mk{8, kb}, mv{(size_t)VSZ, (void*)v};
                if (mdb_put(t, dbi, &mk, &mv, 0) != 0) { std::printf("mdb_put fail\n"); std::exit(1); }
            }
            mdb_txn_commit(t);
        };

        double t0 = now_us();
        put_batch(false);
        li.us = (now_us() - t0) / N;

        t0 = now_us();
        put_batch(true);
        lu.us = (now_us() - t0) / (N / 2);

        t0 = now_us();
        {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, MDB_RDONLY, &t);
            for (u32 i = 0; i < R; ++i) {
                u64 k = keys[mix(i * 7 + 1) % N];
                u8 kb[8]; key_be(k, kb);
                MDB_val mk{8, kb}, mv{};
                mdb_get(t, dbi, &mk, &mv);
            }
            mdb_txn_abort(t);
        }
        lg.us = (now_us() - t0) / R;

        t0 = now_us();
        {
            MDB_txn* t = nullptr;
            mdb_txn_begin(env, nullptr, MDB_RDONLY, &t);
            MDB_cursor* cur = nullptr;
            mdb_cursor_open(t, dbi, &cur);
            for (u32 i = 0; i < S; ++i) {
                u64 lo = keys[mix(i * 13 + 5) % N];
                u64 hi = lo + 4096;
                u8 kb[8]; key_be(lo, kb);
                MDB_val mk{8, kb}, mv{};
                u32 n = 0;
                int rc = mdb_cursor_get(cur, &mk, &mv, MDB_SET_RANGE);
                while (rc == 0 && n < K) {
                    u64 k = be_key(mk.mv_data);
                    if (k >= hi) break;
                    ++n;
                    rc = mdb_cursor_get(cur, &mk, &mv, MDB_NEXT);
                }
            }
            mdb_cursor_close(cur);
            mdb_txn_abort(t);
        }
        lr.us = (now_us() - t0) / (double)S;

        mdb_env_sync(env, 1);
        std::string data = std::string(dpath) + "/data.mdb";
        std::string lock = std::string(dpath) + "/lock.mdb";
        li.space = file_bytes(data.c_str()) + file_bytes(lock.c_str());
        std::printf("# lmdb done insert=%.3f update=%.3f get=%.3f range=%.3f space=%llu B\n",
                    li.us, lu.us, lg.us, lr.us, (unsigned long long)li.space);
        std::fflush(stdout);
        mdb_dbi_close(env, dbi);
        mdb_env_close(env);
    }

    std::printf("\n%-14s %12s %12s %12s\n", "phase", "fcs", "lmdb", "fcs/lmdb");
    auto pr = [](const char* phase, double a, double b) {
        std::printf("%-14s %12.3f %12.3f %12.2f\n", phase, a, b, b ? a / b : 0.0);
    };
    pr("insert us/op", fi.us, li.us);
    pr("update us/op", fu.us, lu.us);
    pr("get us/op",    fg.us, lg.us);
    pr("range us/scan",fr.us, lr.us);
    std::printf("%-14s %12llu %12llu\n", "space bytes",
                (unsigned long long)fi.space, (unsigned long long)li.space);
    return 0;
}
