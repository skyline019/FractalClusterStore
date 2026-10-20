// v1：溢出值（大 value）。> INLINE_VAL_MAX 的值落成溢出页链，页上只留 16 B 描述符。
// 覆盖 1 B ~ 65000 B、恰好跨页边界、覆盖写（大→小 / 小→大）、重开、压实、
// 以及"同一个键反复写 8 KB"时的空间回收（否则溢出页会泄漏）。
// A2 溢出值：各种长度（含跨多页、恰好边界）写入 → 读回 → 重开读回 → 覆盖 → 压实 → 校验
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;
static u8 arena[512 * PAGE];
static u8 pattern(u32 i, u32 j) { return (u8)((i * 131 + j * 7) ^ (j >> 8)); }
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)
int main() {
    static RamDev dev(20000);
    const char* P = "build/v1a.db"; std::remove(P);
    std::vector<u32> lens = {1, 100, 2999, 3000, 3001, 4000, 4080, 8160, 20000, 65000};
    std::vector<u32> exp = lens;                 // 覆盖之后要跟着改
    std::vector<std::vector<u8>> expv(lens.size());
    for (size_t i = 0; i < lens.size(); ++i) {
        expv[i].resize(lens[i]);
        for (u32 j = 0; j < lens[i]; ++j) expv[i][j] = pattern((u32)i, j);
    }
    auto verify_all = [&](Store& s, const char* tag) {
        for (size_t i = 0; i < exp.size(); ++i) {
            std::vector<u8> b(lens[i] + 8);
            u16 l = 0;
            RC rc = s.get((u64)i + 1, b.data(), (u16)b.size(), &l);
            if (rc != RC::OK || l != exp[i]) { std::printf("    %s k=%zu rc=%s len=%u want=%u\n", tag, i, rc_str(rc), l, exp[i]); ++fails; continue; }
            for (u32 j = 0; j < exp[i]; ++j)
                if (b[j] != expv[i][j]) { std::printf("    %s k=%zu byte %u\n", tag, i, j); ++fails; break; }
        }
    };
    {
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open");
        for (size_t i = 0; i < lens.size(); ++i) {
            std::vector<u8> v(lens[i]);
            for (u32 j = 0; j < lens[i]; ++j) v[j] = pattern((u32)i, j);
            RC rc = s.put((u64)i + 1, v.data(), (u16)v.size());
            CHECK(rc == RC::OK, "put");
        }
        CHECK(s.commit() == RC::OK, "commit");
        verify_all(s, "写后");
        // 扫描也要能取到
        int n = 0;
        s.select(0, ~0ull, [&](u64 k, const void* p, u16 l) {
            size_t i = (size_t)k - 1;
            if (i < exp.size()) {
                if (l != exp[i]) ++fails;
                else for (u32 j = 0; j < l; ++j) if (((const u8*)p)[j] != expv[i][j]) { ++fails; break; }
            }
            ++n; return true;
        });
        CHECK(n == (int)lens.size(), "scan 条数");
        // 覆盖：大变小、小变大
        std::vector<u8> v2(5000, 0xAB);
        CHECK(s.put(9, v2.data(), 5000) == RC::OK, "put 9 -> 5000");
        exp[8] = 5000; expv[8] = v2;
        u8 small[16]; std::memset(small, 0x11, sizeof small);
        CHECK(s.put(10, small, 16) == RC::OK, "put 10 -> 16");
        exp[9] = 16; expv[9].assign(small, small + 16);
        CHECK(s.commit() == RC::OK, "commit2");
        u8 big[6000]; u16 l = 0;
        CHECK(s.get(9, big, sizeof big, &l) == RC::OK && l == 5000, "k9 覆盖后长度");
        CHECK(s.get(10, big, sizeof big, &l) == RC::OK && l == 16 && big[0] == 0x11, "k10 覆盖后内容");
        std::printf("写后/覆盖：%s\n", fails ? "有失败" : "OK");
    }
    {   // 重开
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "reopen");
        verify_all(s, "重开");
        u8 big[6000]; u16 l = 0;
        CHECK(s.get(9, big, sizeof big, &l) == RC::OK && l == 5000 && big[0] == 0xAB, "重开后 k9");
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "verify");
        std::printf("重开+verify：%s (used=%u runs=%u)\n", fails ? "有失败" : "OK", s.used_pages(), s.run_count());
    }
    {   // 压实：溢出值必须整体搬（链跟着记录走）
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open3");
        CHECK(s.consolidate(0) == RC::OK, "consolidate");
        verify_all(s, "压实后");
        s.checkpoint();
        Report r; s.verify(&r);
        if (r.errors) std::printf("    verify: errs=%u first=%s key=%llu recs=%u ovfl=%u used=%u\n",
                                  r.errors, r.first_err, (unsigned long long)r.first_key, r.recs, r.ovfl, s.used_pages());
        CHECK(r.errors == 0, "压实 verify");
        std::printf("压实：%s\n", fails ? "有失败" : "OK");
    }
    {   // 再重开一次
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open4");
        verify_all(s, "压实后重开");
        std::printf("压实后重开：%s\n", fails ? "有失败" : "OK");
    }
    {   // 反复覆盖同一个键（大值）：溢出链必须被回收，used 不能单调上涨
        const char* P2 = "build/v1b.db"; std::remove(P2);
        FileDev fd(P2, 8000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "churn open");
        std::vector<u8> v(8000, 0x5A);
        u32 u0 = 0, umax = 0;
        for (int r = 0; r < 40; ++r) {
            v[0] = (u8)r;
            CHECK(s.put(7, v.data(), 8000) == RC::OK, "churn put");
            CHECK(s.commit() == RC::OK, "churn commit");
            u32 u = s.used_pages();
            if (!u0) u0 = u;
            if (u > umax) umax = u;
        }
        Report r2; s.verify(&r2);
        CHECK(r2.errors == 0, "churn verify");
        u8 b[16]; u16 l = 0;
        CHECK(s.get(7, b, sizeof b, &l) == RC::OK && l == 8000 && b[0] == 39, "churn 内容");
        std::printf("大值覆盖 40 轮：used 首轮 %u → 当前 %u（峰值 %u）\n", u0, s.used_pages(), umax);
        CHECK(s.used_pages() <= u0 * 2 + 16, "大值覆盖后空间应回落");
        u32 before = s.used_pages();
        // 关掉大值：>3000 B 必须拒绝（老行为），而 ≤3000 B 照常
        s.set_overflow(false);
        std::vector<u8> big(4000, 1);
        CHECK(s.put(11, big.data(), 4000) == RC::RANGE, "关掉溢出后 >3000 B 应 RANGE");
        std::vector<u8> ok3000(3000, 2);
        CHECK(s.put(11, ok3000.data(), 3000) == RC::OK, "3000 B 仍然可以");
        CHECK(s.commit() == RC::OK, "commit");
        s.set_overflow(true);
        // 重开：溢出页必须被 rebuild_free 认作"已用"，否则再写会把它们发出去
        u8 b2[8]; u16 l2 = 0;
        CHECK(s.get(7, b2, sizeof b2, &l2) == RC::OK && l2 == 8000 && b2[0] == 39, "churn 内容(前)");
        std::printf("故障注入/关开关：OK（used %u）\n", before);
    }
    {   // 重开：used 账必须精确，且大值仍可读
        const char* P2 = "build/v1b.db";
        FileDev fd(P2, 8000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "churn reopen");
        u8 b[16]; u16 l = 0;
        CHECK(s.get(7, b, sizeof b, &l) == RC::OK && l == 8000 && b[0] == 39, "重开后 churn 内容");
        CHECK(s.get(11, b, sizeof b, &l) == RC::OK && l == 3000, "重开后 3000 B");
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "churn reopen verify");
        // 再写一批，验证分配器没有把溢出页误当空闲
        std::vector<u8> v(6000, 0x77);
        for (int i = 0; i < 10; ++i) { CHECK(s.put(1000 + i, v.data(), 6000) == RC::OK, "重开后再写"); }
        CHECK(s.commit() == RC::OK, "commit");
        CHECK(s.get(7, b, sizeof b, &l) == RC::OK && l == 8000, "重开后老值没被覆盖");
        Report r3; s.verify(&r3);
        CHECK(r3.errors == 0, "重开再写 verify");
        std::printf("大值重开+再写：OK（used=%u, ovfl=%u）\n", s.used_pages(), r3.ovfl);
    }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
