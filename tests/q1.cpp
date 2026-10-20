// q1.cpp — 查询层：游标、范围/前缀选择、二级索引、分页、MVCC 交互
#include "fcs.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <algorithm>
#include <random>

using namespace xdb;
using namespace fcs;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); ++fails; } } while (0)

static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}

// 用 select 拉出整个键空间，和参照模型逐条比对
static void cmp_select(Store& s, const std::map<u64, u64>& ref, u64 lo, u64 hi) {
    size_t i = 0; bool ok = true;
    std::vector<std::pair<u64,u64>> got;
    s.select(lo, hi, [&](u64 k, const u8* v, u16 l) {
        u64 val = 0; if (l == 8) std::memcpy(&val, v, 8);
        got.push_back({k, val}); return true;
    });
    for (auto& kv : ref) {
        if (kv.first < lo || kv.first >= hi) continue;
        if (i >= got.size()) { ok = false; break; }
        if (got[i].first != kv.first || got[i].second != kv.second) { ok = false; break; }
        ++i;
    }
    if (ok && i != got.size()) ok = false;
    CHECK(ok, "select 与参照模型不一致");
    if (!ok) {
        std::printf("   ref=%zu got=%zu  [%llx,%llx)\n", i, got.size(),
                    (unsigned long long)lo, (unsigned long long)hi);
        size_t extra = 0;
        for (auto& g : got) {
            if (ref.count(g.first)) continue;
            if (++extra <= 6)
                std::printf("     EXTRA k=%016llx v=%llx\n", (unsigned long long)g.first,
                            (unsigned long long)g.second);
        }
        std::printf("     extra_rows=%zu\n", extra);
        size_t j = 0;
        for (auto& kv : ref) {
            if (kv.first < lo || kv.first >= hi) continue;
            if (j > 8) break;
            std::printf("     ref k=%016llx v=%llx | got k=%016llx v=%llx\n",
                (unsigned long long)kv.first, (unsigned long long)kv.second,
                (unsigned long long)got[j].first, (unsigned long long)got[j].second);
            ++j;
        }
    }
}

int main() {
    const u32 NP = 12288;                       // 48 MB
    RamDev dev(NP);
    Store s;
    CHECK(s.open(&dev, nullptr, 512) == RC::OK, "open");

    std::map<u64, u64> ref;
    std::mt19937_64 rng(12345);

    // ---------------------------------------------------------- 1. 写入随机键
    const u32 N = getenv("Q1N") ? (u32)atoi(getenv("Q1N")) : 30000;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (size_t i = 0; i < keys.size(); ++i) {
        u64 v = mix(keys[i] * 7 + 1);
        CHECK(s.put(keys[i], &v, 8) == RC::OK, "put");
        ref[keys[i]] = v;
        if ((i + 1) % 500 == 0) CHECK(s.commit() == RC::OK, "commit");
    }
    CHECK(s.commit() == RC::OK, "commit tail");
    std::printf("inserted=%zu cells=%u\n", ref.size(), s.cell_count()); std::fflush(stdout);

    // 删除每 7 个键 => 墓碑必须对查询不可见
    size_t del = 0;
    for (size_t i = 0; i < keys.size(); i += 7) {
        CHECK(s.del(keys[i]) == RC::OK, "del");
        ref.erase(keys[i]); ++del;
        if (del % 500 == 0) CHECK(s.commit() == RC::OK, "commit del");
    }
    CHECK(s.commit() == RC::OK, "commit del tail");
    std::printf("deleted=%zu live=%zu\n", del, ref.size());; std::fflush(stdout);

    cmp_select(s, ref, 0, ~0ull);
    cmp_select(s, ref, mix(0) & SKEY_MAX, mix(N / 3) & SKEY_MAX);
    cmp_select(s, ref, 0, mix(64) & SKEY_MAX);
    CHECK(s.count(0, ~0ull) == (u32)ref.size(), "count");

    // ---------------------------------------------------------- 2. limit/offset 分页
    {
        std::vector<u64> all;
        s.select(0, ~0ull, [&](u64 k, const u8*, u16) { all.push_back(k); return true; });
        const u32 PS = 97;                       // 故意用非 2 的幂做页大小
        size_t pos = 0;
        bool ok = true;
        for (u32 page = 0; page * PS < all.size(); ++page) {
            std::vector<u64> got;
            s.select(0, ~0ull, [&](u64 k, const u8*, u16) { got.push_back(k); return true; },
                     PS, page * PS);
            for (size_t j = 0; j < got.size(); ++j)
                if (pos + j >= all.size() || got[j] != all[pos + j]) { ok = false; break; }
            pos += got.size();
            if (!ok) break;
        }
        CHECK(ok && pos == all.size(), "分页 offset/limit 与全量一致");
        std::printf("paged=%zu pages=%zu\n", all.size(), (all.size() + PS - 1) / PS); std::fflush(stdout);
    }

    // ---------------------------------------------------------- 3. 游标可暂停/重定位
    {
        Store::Cursor cur;
        CHECK(cur.open(&s, 0, ~0ull), "cursor open");
        u64 k; const u8* v; u16 l;
        size_t n = 0; bool ok = true;
        while (cur.next(k, v, l)) {              // 每条之间插入一次随机点查，检验游标可暂停
            if (n % 97 == 0) {
                u64 probe = keys[rng() % keys.size()], got = 0; u16 gl = 0;
                RC rc = s.get(probe, &got, 8, &gl);
                auto it = ref.find(probe);
                if (it == ref.end()) { if (rc == RC::OK) ok = false; }
                else if (rc != RC::OK || got != it->second) ok = false;
            }
            ++n;
        }
        CHECK(ok, "游标暂停期间点查正确");
        CHECK(n == ref.size(), "游标条数与参照一致");
        // 重定位：seek 到中位数，后面必须还是键序
        u64 mid = keys[keys.size() / 2];
        CHECK(cur.seek(mid), "cursor seek");
        u64 prev = 0; size_t m = 0;
        while (cur.next(k, v, l)) { CHECK(k >= prev, "seek 后仍为升序"); prev = k; ++m; }
        std::printf("cursor n=%zu seek_from=%zu rest=%zu\n", n, (size_t)mid, m);
    }

    // ---------------------------------------------------------- 4. 前缀选择
    {
        // 前缀 = 高 12 位
        std::map<u64, size_t> cnt;
        for (auto& kv : ref) cnt[kv.first >> 56] += 1;
        bool ok = true;
        for (auto& c : cnt) {
            size_t n = 0;
            s.select_prefix(c.first, 8, [&](u64, const u8*, u16) { ++n; return true; });
            if (n != c.second) { ok = false; std::printf("  pfx=%llx ref=%zu got=%zu\n",
                                    (unsigned long long)c.first, c.second, n); }
        }
        CHECK(ok, "前缀扫描条数");
        std::printf("prefixes=%zu\n", cnt.size());
    }

    // ---------------------------------------------------------- 5. 二级索引
    {
        // 索引 0：按 value 排序（模拟 ORDER BY non-key column）
        // 索引 1：按 key 的低 16 位
        size_t i = 0;
        for (auto& kv : ref) {
            CHECK(s.index_put(0, kv.second & SKEY_MAX, kv.first) == RC::OK, "index_put");
            CHECK(s.index_put(1, kv.first & 0xFFFF, kv.first) == RC::OK, "index_put2");
            if (++i % 500 == 0) CHECK(s.commit() == RC::OK, "index commit");
        }
        CHECK(s.commit() == RC::OK, "index commit tail");

        // 索引范围扫描 + 回表，结果必须与参照模型一致
        std::vector<std::pair<u64,u64>> refv;   // (skey, pk)
        for (auto& kv : ref) refv.push_back({kv.second & SKEY_MAX, kv.first});
        std::sort(refv.begin(), refv.end());
        u64 lo = refv[refv.size() / 4].first, hi = refv[refv.size() / 2].first;

        std::vector<std::pair<u64,u64>> got;
        s.index_scan(0, lo, hi, [&](u64 skey, u64 pk) {
            u64 v = 0; u16 l = 0;
            CHECK(s.get(pk, &v, 8, &l) == RC::OK, "回表");
            got.push_back({skey, pk}); return true;
        });
        size_t expect = 0;
        bool ok = true;
        for (auto& e : refv) {
            if (e.first < lo || e.first >= hi) continue;
            if (expect >= got.size() || got[expect] != e) { ok = false; break; }
            ++expect;
        }
        if (expect != got.size()) ok = false;
        CHECK(ok, "索引范围扫描+回表");
        std::printf("index range [%llu,%llu) rows=%zu (ref=%zu)\n",
                    (unsigned long long)lo, (unsigned long long)hi, got.size(), expect);

        u64 pk = 0;
        CHECK(s.index_get(0, refv[10].first, pk) == RC::OK && pk == refv[10].second, "index_get");
        // 删除索引条目后不可见
        CHECK(s.index_del(0, refv[10].first) == RC::OK, "index_del");
        CHECK(s.commit() == RC::OK, "index del commit");
        CHECK(s.index_get(0, refv[10].first, pk) == RC::NOTFOUND, "index_get after del");
        // 索引键不污染用户键空间
        CHECK(s.count(0, ~0ull) == (u32)ref.size(), "索引不进主键空间");
        CHECK(s.put(IDX_BASE + 1, "x", 1) == RC::RANGE, "用户不得写入索引空间");
    }

    // ---------------------------------------------------------- 6. 快照 + 查询
    {
        u32 snap = s.last_commit();
        CHECK(s.snapshot() == RC::OK, "snapshot");   // 打开快照 ⇒ 压实/退役暂停
        for (size_t i = 0; i < 200; ++i) {
            u64 k = keys[i * 31 % keys.size()], v = 0xDEAD0000 + i;
            s.put(k, &v, 8);
        }
        CHECK(s.commit() == RC::OK, "commit under snap");
        // 快照读：同一视图下前后两次扫描必须一致（稳定的历史）
        std::vector<std::pair<u64,u64>> a1, a2;
        s.select_at(snap, 0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            u64 val = 0; if (l == 8) std::memcpy(&val, v, 8); a1.push_back({k, val}); return true; });
        {
            size_t nn = 0, rr = 0;
            s.select_at(snap, 0, IDX_BASE, [&](u64, const u8*, u16) { ++nn; return true; });
            for (auto& kv : ref) if (kv.first < IDX_BASE) ++rr;
            std::printf("snapshot rows=%zu ref=%zu\n", nn, rr);
            CHECK(nn == rr, "快照视图行数 = 旧模型");
        }
        s.select_at(snap, 0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            u64 val = 0; if (l == 8) std::memcpy(&val, v, 8); a2.push_back({k, val}); return true; });
        CHECK(a1 == a2, "同一快照两次扫描一致");
        CHECK(s.forget_snapshots() == RC::OK, "forget_snapshots");
        // 当前读：看到新值
        for (size_t i = 0; i < 200; ++i) {
            u64 k = keys[i * 31 % keys.size()];
            u64 v = 0xDEAD0000 + i; ref[k] = v;
        }
        cmp_select(s, ref, 0, ~0ull);
        std::printf("snapshot query OK\n");
    }

    // ---------------------------------------------------------- 7. 重新打开后查询
    {
        std::vector<std::pair<u64,u64>> before;
        s.select(0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            u64 val = 0; if (l == 8) std::memcpy(&val, v, 8);
            before.push_back({k, val}); return true;
        });
        u32 nidx = 0;
        s.index_scan(1, 0, 0x10000, [&](u64, u64) { ++nidx; return true; });
        CHECK(s.open(&dev, nullptr, 512) == RC::OK, "reopen");
        std::vector<std::pair<u64,u64>> after;
        s.select(0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            u64 val = 0; if (l == 8) std::memcpy(&val, v, 8);
            after.push_back({k, val}); return true;
        });
        CHECK(before == after, "重开后主键扫描一致");
        u32 nidx2 = 0;
        s.index_scan(1, 0, 0x10000, [&](u64, u64) { ++nidx2; return true; });
        CHECK(nidx == nidx2, "重开后索引扫描一致");
        cmp_select(s, ref, 0, IDX_BASE);
        std::printf("reopen query: rows=%zu idx2=%u\n", after.size(), nidx2);
    }

    // ---------------------------------------------------------- 8. 与 scan() 的一致性
    {
        std::vector<std::pair<u64,u64>> a, b;
        s.scan([&](u64 k, const u8* v, u16 l) {
            if (k >= IDX_BASE) return true;
            u64 val = 0; if (l == 8 && v) std::memcpy(&val, v, 8); a.push_back({k, val}); return true; });
        s.select(0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            u64 val = 0; if (l == 8) std::memcpy(&val, v, 8); b.push_back({k, val}); return true; });
        CHECK(a == b, "scan() 与 select() 一致");
    }

    // ---------------------------------------------------------- 9. 组提交
    {
        RamDev d2(2048);
        Store g;
        CHECK(g.open(&d2, nullptr, 256) == RC::OK, "group open");
        u64 w0 = d2.st.write_bytes;
        for (u32 b = 0; b < 5; ++b) {                  // 5 次逻辑提交，0 字节 I/O
            for (u32 i = 0; i < 20; ++i) {
                u64 k = 1000 + b * 20 + i, v = k * 3;
                CHECK(g.put(k, &v, 8) == RC::OK, "group put");
            }
            CHECK(g.commit(false) == RC::OK, "commit(false)");
        }
        CHECK(d2.st.write_bytes == w0, "非持久提交不写盘");
        CHECK(g.pending_commits() == 5, "pending_commits");
        u64 v = 0; u16 l = 0;
        CHECK(g.get(1000, &v, 8, &l) == RC::OK && v == 3000, "逻辑提交后即可读");
        CHECK(g.commit() == RC::OK, "组提交落盘");
        CHECK(d2.st.write_bytes > w0, "落盘产生写");
        // abort() 不得丢弃已逻辑提交的数据
        for (u32 i = 0; i < 10; ++i) { u64 k = 5000 + i; g.put(k, &k, 8); }
        CHECK(g.abort() == RC::OK, "abort");
        CHECK(g.get(5000, &v, 8, &l) == RC::NOTFOUND, "abort 丢弃未提交写");
        CHECK(g.get(1019, &v, 8, &l) == RC::OK, "abort 保留已提交写");
        CHECK(g.open(&d2, nullptr, 256) == RC::OK, "group reopen");
        u32 n = 0;
        g.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n; return true; });
        CHECK(n == 100, "组提交后重开数据完整");
        Report rep;
        CHECK(g.verify(&rep) == RC::OK, "组提交 verify");
        std::printf("group commit: 5 logical -> 1 physical, rows=%u\n", n);
    }

    // ---------------------------------------------------------- 10. 异步页写
    {
        RamDev d3(4096);
        Store a;
        CHECK(a.open(&d3, nullptr, 512) == RC::OK, "async open");
        a.set_async_writes(true);
        std::map<u64, u64> m;
        for (u32 round = 0; round < 40; ++round) {
            for (u32 i = 0; i < 100; ++i) {
                u64 k = (round * 100 + i) * 17 % 5000 + 1;
                u64 v = round * 1000 + i;
                CHECK(a.put(k, &v, 8) == RC::OK, "async put");
                m[k] = v;
            }
            CHECK(a.commit() == RC::OK, "async commit");
            // 提交之间做点读，验证读写不会撞车
            if (round % 7 == 0) {
                for (auto& kv : m) {
                    u64 got = 0; u16 gl = 0;
                    CHECK(a.get(kv.first, &got, 8, &gl) == RC::OK && got == kv.second, "异步期间的读");
                    break;
                }
            }
        }
        u64 bad = 0;
        for (auto& kv : m) {
            u64 got = 0; u16 gl = 0;
            if (a.get(kv.first, &got, 8, &gl) != RC::OK || got != kv.second) ++bad;
        }
        CHECK(bad == 0, "异步写：数据全对");
        Report rep;
        CHECK(a.verify(&rep) == RC::OK, "异步写 verify");
        u64 rows = 0, want = m.size();
        a.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++rows; return true; });
        CHECK(rows == want, "异步写：扫描条数");
        // 非持久提交 + 后台落页：读必须能看见，掉电只丢未 sync 的那部分
        {
            RamDev d4(2048);
            Store b;
            CHECK(b.open(&d4, nullptr, 512) == RC::OK, "async2 open");
            b.set_async_writes(true);
            b.set_async_flush_min(16);
            b.set_io_threads(3);                     // 多写线程也要一致
            std::map<u64, u64> mm;
            for (u32 i = 0; i < 400; ++i) {
                u64 k = i * 7 + 3, v = i;
                CHECK(b.put(k, &v, 8) == RC::OK, "async2 put");
                mm[k] = v;
                if (i % 20 == 19) CHECK(b.commit(false) == RC::OK, "async2 逻辑提交");
            }
            // 未落提交点前，本进程内读必须一致（run 已挂上）
            u64 bad2 = 0;
            for (auto& kv : mm) {
                u64 got = 0; u16 gl = 0;
                if (b.get(kv.first, &got, 8, &gl) != RC::OK || got != kv.second) ++bad2;
            }
            CHECK(bad2 == 0, "async2 逻辑提交后即可读");
            u64 hw = b.mem_high_water();
            CHECK(hw < 64 * 1024, "async2 工作区被压住");
            CHECK(b.commit() == RC::OK, "async2 持久提交");
            u32 n3 = 0;
            b.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n3; return true; });
            CHECK(n3 == mm.size(), "async2 提交后条数");
            CHECK(b.open(&d4, nullptr, 512) == RC::OK, "async2 reopen");
            n3 = 0;
            b.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n3; return true; });
            CHECK(n3 == mm.size(), "async2 重开后条数");
            Report r2;
            CHECK(b.verify(&r2) == RC::OK, "async2 verify");
            // 增量归一 + 收缩：只碰碎片化的单元，然后把文件收回去
            CHECK(b.consolidate(2) == RC::OK, "增量归一");
            CHECK(b.compact_file() == RC::OK, "归一+搬家+检查点+收缩");
            n3 = 0;
            b.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n3; return true; });
            CHECK(n3 == mm.size(), "收缩后条数");
            Report r3;
            CHECK(b.verify(&r3) == RC::OK, "收缩后 verify");
            CHECK(b.open(&d4, nullptr, 512) == RC::OK, "收缩后 reopen");
            n3 = 0;
            b.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n3; return true; });
            CHECK(n3 == mm.size(), "收缩后重开条数");
        }

        a.set_async_writes(false);
        CHECK(a.open(&d3, nullptr, 512) == RC::OK, "async reopen");
        u32 n2 = 0;
        a.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n2; return true; });
        CHECK(n2 == want, "异步写：重开后完整");
        std::printf("async: rows=%llu io_jobs=%llu\n", (unsigned long long)rows, (unsigned long long)a.io_done());
    }

    std::printf(fails ? "FAILURES:%d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
