// g3 —— 第 7 轮回归：范围墓碑 × 存活账 × 内存表 × 链缓存
//   1) 同 lo 的两条墓碑（宽的在前、窄的在后）：被宽墓碑盖住的记录**不能复活**（坑 62），重开也一样
//   2) 长删循环：del_range 半库 → 提交 → 回填 → 提交，每步 verify；被盖住的整条 run 必须在**本次**提交退休
//      （坑 63：以前校准只看"有没有更新版本"，被墓碑盖住的 run 永远 live=nrec，runs 20→138）
//   3) 未提交的大值 get 必须返回整个值，不是 16 B 描述符（坑 64）
//   4) 链缓存：重复 get 命中；覆盖 / 回收 / 页复用之后读到的必须是新值（按页区间失效）
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static int t_same_lo() {
    static u8 arena[64 * PAGE];
    const char* P = "build/g3_a.db"; std::remove(P);
    FileDev fd(P, 4000);
    {
        Store s; if (s.open(&fd, arena, 64) != RC::OK) return 1;
        u8 v[16] = {1};
        for (u64 k = 1; k <= 100; ++k) s.put(k, v, 16);
        s.commit();
        s.del_range(10, 90); s.commit();
        CHECK(s.count(1, 101) == 20, "after [10,90): %u", s.count(1, 101));
        s.del_range(10, 20); s.commit();                  // 同 lo、更窄、版本更新
        CHECK(s.count(1, 101) == 20, "after [10,20): %u (复活了)", s.count(1, 101));
        u16 l; CHECK(s.get(50, v, 16, &l) == RC::NOTFOUND, "get(50) 应 NOTFOUND");
        CHECK(s.range_tombstones() == 2, "tomb=%u", s.range_tombstones());
        // 支配规则：同 lo、更宽、版本更新 ⇒ 顶掉前两条
        s.del_range(10, 95); s.commit();
        CHECK(s.range_tombstones() == 1, "支配后 tomb=%u", s.range_tombstones());
        CHECK(s.count(1, 101) == 15, "after [10,95): %u", s.count(1, 101));
        Report r; s.verify(&r); CHECK(r.errors == 0, "verify %s", r.first_err);
    }
    Store s2; if (s2.open(&fd, arena, 64) != RC::OK) return 1;
    CHECK(s2.count(1, 101) == 15, "reopen visible=%u", s2.count(1, 101));
    CHECK(s2.range_tombstones() == 1, "reopen tomb=%u", s2.range_tombstones());
    std::printf("同 lo 墓碑：visible=%u tomb=%u\n", s2.count(1, 101), s2.range_tombstones());
    return 0;
}

static int t_delloop() {
    static u8 arena[256 * PAGE];
    const char* P = "build/g3_b.db"; std::remove(P);
    FileDev fd(P, 100000);
    Store s; if (s.open(&fd, arena, 256) != RC::OK) return 1;
    // 第 13 轮：默认的 cheap 规则会把 10 条 3000 键的刷出并成 1 + 2 条，于是"删一半"再也盖不住任何一整条 run，
    // 本段要验的是"整条被盖住的 run 当次提交退休"，关掉它保持老布局（每次刷出一条 run）。
    s.set_merge_cheap_pages(0);
    const int n = 30000;
    std::vector<u8> v(100, 0x33);
    auto fill = [&](u64 lo, u64 hi, u8 tag) {
        for (u64 k = lo; k < hi; ++k) {
            std::memcpy(v.data(), &k, 8); v[8] = tag;
            s.put(k, v.data(), 100);
            if ((k - lo) % 3000 == 2999) s.commit();
        }
        s.commit();
    };
    fill(1, n + 1, 0);
    const u32 runs0 = s.run_count();
    u32 max_runs = runs0;
    Report r;
    for (int round = 0; round < 4; ++round) {
        u64 lo = (round & 1) ? (u64)n / 2 + 1 : 1, hi = (round & 1) ? (u64)n + 1 : (u64)n / 2 + 1;
        u32 runs_before = s.run_count();
        s.del_range(lo, hi);
        s.commit();
        // 整条被盖住的 run 必须在这次提交就退休（不是下一次）
        CHECK(s.run_count() < runs_before, "round %d: del 提交后 runs %u → %u（被盖住的 run 没退休）", round, runs_before, s.run_count());
        s.verify(&r); CHECK(r.errors == 0, "round %d del: %s", round, r.first_err);
        CHECK(s.count(1, n + 1) == (u32)(n - (hi - lo)), "round %d visible=%u", round, s.count(1, n + 1));
        fill(lo, hi, (u8)(round + 1));
        s.verify(&r); CHECK(r.errors == 0, "round %d fill: %s", round, r.first_err);
        CHECK(s.count(1, n + 1) == (u32)n, "round %d refilled visible=%u", round, s.count(1, n + 1));
        max_runs = std::max(max_runs, s.run_count());
    }
    CHECK(max_runs <= runs0 + 2, "runs 失控：初始 %u，峰值 %u", runs0, max_runs);
    // 回读抽查：值是最后一轮的 tag
    u8 out[100]; u16 l;
    CHECK(s.get(5, out, 100, &l) == RC::OK && l == 100 && out[8] == 3, "key 5 tag=%u", out[8]);
    CHECK(s.get((u64)n - 5, out, 100, &l) == RC::OK && l == 100 && out[8] == 4, "key n-5 tag=%u", out[8]);
    std::printf("长删循环：runs 初始 %u / 峰值 %u / 最终 %u，used=%u live=%llu tomb=%u\n", runs0, max_runs, s.run_count(),
                s.used_pages(), (unsigned long long)s.live_page_count(), s.range_tombstones());
    return 0;
}

static int t_mem_bigval() {
    static u8 arena[64 * PAGE];
    const char* P = "build/g3_c.db"; std::remove(P);
    FileDev fd(P, 4000);
    Store s; if (s.open(&fd, arena, 64) != RC::OK) return 1;
    std::vector<u8> v(8000, 0xAB), out(8000, 0);
    s.put(7, v.data(), 8000);
    u16 l = 0;
    CHECK(s.get(7, out.data(), 8000, &l) == RC::OK && l == 8000 && out[0] == 0xAB && out[7999] == 0xAB,
          "未提交大值 get: len=%u first=%02x", l, out[0]);
    // 同批覆盖成小值再覆盖成大值，再删
    u8 small[8] = {9};
    s.put(7, small, 8);
    CHECK(s.get(7, out.data(), 8000, &l) == RC::OK && l == 8, "覆盖成小值 len=%u", l);
    v[0] = 0xCD; s.put(7, v.data(), 8000);
    CHECK(s.get(7, out.data(), 8000, &l) == RC::OK && l == 8000 && out[0] == 0xCD, "再覆盖成大值 first=%02x", out[0]);
    s.del(7);
    CHECK(s.get(7, out.data(), 8000, &l) == RC::NOTFOUND, "同批删除后应 NOTFOUND");
    v[0] = 0xEF; s.put(7, v.data(), 8000);
    s.commit();
    CHECK(s.get(7, out.data(), 8000, &l) == RC::OK && l == 8000 && out[0] == 0xEF, "提交后 first=%02x", out[0]);
    Report r; s.verify(&r); CHECK(r.errors == 0, "verify %s", r.first_err);
    std::printf("内存表大值：OK（ovfl=%u used=%u）\n", r.ovfl, s.used_pages());
    return 0;
}

static int t_ovfl_cache() {
    static u8 arena[128 * PAGE];
    const char* P = "build/g3_d.db"; std::remove(P);
    FileDev fd(P, 20000);
    Store s; if (s.open(&fd, arena, 128) != RC::OK) return 1;
    std::vector<u8> a(8000, 0xA1), b(8000, 0xB2), out(8000);
    for (u64 k = 1; k <= 40; ++k) { a[1] = (u8)k; s.put(k, a.data(), 8000); }
    s.commit();
    u16 l;
    // 重复读：第二次起命中
    for (int i = 0; i < 4; ++i) CHECK(s.get(3, out.data(), 8000, &l) == RC::OK && out[0] == 0xA1 && out[1] == 3, "get 3");
    CHECK(s.ovfl_cache_hits() >= 2, "hits=%llu", (unsigned long long)s.ovfl_cache_hits());
    // 覆盖：新链、旧链回收；读到的必须是新值
    b[1] = 3; s.put(3, b.data(), 8000); s.commit();
    CHECK(s.get(3, out.data(), 8000, &l) == RC::OK && out[0] == 0xB2, "覆盖后 first=%02x", out[0]);
    // 归一 + 大量新写让旧链的页被复用，再读 3 与新键
    s.consolidate(0);
    for (u64 k = 100; k < 140; ++k) { a[1] = (u8)k; s.put(k, a.data(), 8000); }
    s.commit();
    CHECK(s.get(3, out.data(), 8000, &l) == RC::OK && out[0] == 0xB2 && out[1] == 3, "页复用后 key3 first=%02x", out[0]);
    CHECK(s.get(120, out.data(), 8000, &l) == RC::OK && out[0] == 0xA1 && out[1] == 120, "新键 120 [1]=%u", out[1]);
    // 全部重读两遍：值都对；缓存可关
    u64 bad = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (u64 k = 1; k <= 40; ++k) {
            if (s.get(k, out.data(), 8000, &l) != RC::OK || l != 8000) { ++bad; continue; }
            if (out[1] != (u8)k || out[0] != (k == 3 ? 0xB2 : 0xA1)) ++bad;
        }
    CHECK(bad == 0, "重读 bad=%llu", (unsigned long long)bad);
    s.set_ovfl_cache_bytes(0);
    CHECK(s.get(3, out.data(), 8000, &l) == RC::OK && out[0] == 0xB2, "关缓存后读 3");
    Report r; s.verify(&r); CHECK(r.errors == 0, "verify %s", r.first_err);
    // 回滚到快照之前的页复用：快照 → 覆盖 → 回滚 → 读旧值（缓存必须随 reload 清空）
    s.set_ovfl_cache_bytes(256 * 1024);
    s.snapshot();
    s.get(5, out.data(), 8000, &l);                      // 进缓存
    b[1] = 5; s.put(5, b.data(), 8000); s.commit();
    CHECK(s.get(5, out.data(), 8000, &l) == RC::OK && out[0] == 0xB2, "快照后覆盖 5");
    CHECK(s.rollback(0) == RC::OK, "rollback");
    CHECK(s.get(5, out.data(), 8000, &l) == RC::OK && out[0] == 0xA1 && out[1] == 5, "回滚后 5 first=%02x", out[0]);
    s.forget_snapshots();
    std::printf("链缓存：hits=%llu misses=%llu，覆盖/复用/回滚后读值正确\n",
                (unsigned long long)s.ovfl_cache_hits(), (unsigned long long)s.ovfl_cache_misses());
    return 0;
}

int main() {
    if (t_same_lo()) return 1;
    if (t_delloop()) return 1;
    if (t_mem_bigval()) return 1;
    if (t_ovfl_cache()) return 1;
    std::printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
