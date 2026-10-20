// g4：第 7 轮回归（二）—— 异步写 × 快照/回退 × 压实布局。
//   这三条都是 s2 长跑里潜伏了两轮的旧 bug：s2 每次回退后 resync 真值，把它们全部藏住了。
//   1. 坑 67：prefetch 绕过 wait_page —— 异步提交刚写出的 run 还在飞，relocate 的单游标扫描
//      把该位置的旧页/零页预取进缓存 ⇒ 重写出的 run 丢整页记录（用慢写设备把窗口放大成确定性）。
//   2. 坑 68：commit(false) 刷出的 run 的 M_RUN_ADD 暂存在 pend_delta_；consolidate 直接走
//      commit_delta 先把它合并掉（DEL 入账），下一次 commit() 才把过期 ADD 追加 ⇒ 重开复活僵尸 run。
//   3. 坑 69：snapshot() 只抓 pend_，不抓 pend_delta_/mem_ 里的逻辑提交 ⇒ 回退丢掉已可见的写。
//   4. 布局：compact_file 把 run 搬到低位后，checkpoint 的清单页若按"最低空闲页"分配会卡进
//      低位大洞，洞 = run 块 − 1 页 ⇒ 永远搬不下去（used 2× live）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
using namespace fcs;

static int fails = 0;
static Store* g_tr = nullptr;      // A4：第一次 CHECK 失败时把追踪环倒出来
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL %s\n", msg); if (++fails == 1 && g_tr) g_tr->trace_dump(stderr, 300); } } while (0)

// 慢写设备：slow_us > 0 时每次页写睡一下，让"异步写在飞"的窗口变成确定性的。
struct SlowDev : RamDev {
    std::atomic<int> slow_us{0};
    explicit SlowDev(PageId n) : RamDev(n) {}
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        int us = slow_us.load();
        if (us) std::this_thread::sleep_for(std::chrono::microseconds(us));
        return RamDev::do_write(pid, off, src, len);
    }
};

static void fillv(std::vector<u8>& v, u64 k, int gen) {
    for (size_t i = 0; i < v.size(); ++i) v[i] = (u8)(k * 31 + i * 7 + gen * 131);
}
static u32 readback(Store& s, u64 k0, u64 k1, int gen, u16 len) {
    std::vector<u8> want(len), got(PAGE);
    u32 bad = 0;
    for (u64 k = k0; k <= k1; ++k) {
        u16 l = 0;
        RC rc = s.get(k, got.data(), PAGE, &l);
        fillv(want, k, gen);
        if (rc != RC::OK || l != len || std::memcmp(got.data(), want.data(), len) != 0) ++bad;
    }
    return bad;
}

int main() {
    // ---------- 1. 坑 67：在飞的 run 被 select 的预取读成旧页 ----------
    {
        static u8 arena[256 * PAGE];
        static SlowDev dev(4096);
        Store s; g_tr = &s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
        s.set_async_writes(true);
        s.set_io_threads(2);
        s.set_async_flush_min(16);
        std::vector<u8> v(400);
        // 先让这片页区装过别的数据（旧页），再收缩回来：之后异步写出的 run 会落在同一片页上
        for (u64 k = 1; k <= 3000; ++k) { fillv(v, k, 1); s.put(k, v.data(), (u16)v.size()); }
        CHECK(s.commit() == RC::OK, "commit gen1");
        for (u64 k = 1; k <= 3000; ++k) s.del(k);
        CHECK(s.commit() == RC::OK, "commit del");
        CHECK(s.compact_file() == RC::OK, "compact_file");
        // 现在：3000 个新值 → commit(false) 把 run 丢给后台线程（慢写：每页 300 µs，~300 页 ≈ 50 ms 在飞）
        // → 立刻 select 整个键空间。select 的游标顺序扫 + 预取：预取若不看 hang_，读到的就是旧页/零页。
        for (u64 k = 1; k <= 3000; ++k) { fillv(v, k, 2); s.put(k, v.data(), (u16)v.size()); }
        dev.slow_us = 300;
        CHECK(s.commit(false) == RC::OK, "commit(false)");
        u32 rows = 0, badrows = 0; std::vector<u8> want(400);
        s.select(1, 3001, [&](u64 k, const u8* val, u16 len) {
            ++rows; fillv(want, k, 2);
            if (len != 400 || std::memcmp(val, want.data(), 400) != 0) ++badrows;
            return true;
        });
        dev.slow_us = 0;
        u32 bad = readback(s, 1, 3000, 2, 400);          // 预取塞进缓存的旧页会一直留着：get 也会中招
        CHECK(s.commit() == RC::OK, "commit");
        Report r; s.verify(&r);
        std::printf("在飞 run 的 select：rows=%u/3000 坏行=%u 之后 get 坏=%u verify=%s\n", rows, badrows, bad, r.errors ? r.first_err : "OK");
        CHECK(rows == 3000 && badrows == 0, "select 不读到在飞页的旧内容（坑 67）");
        CHECK(bad == 0, "预取不污染页缓存（坑 67）");
        CHECK(r.errors == 0, "verify");
    }

    // ---------- 2. 坑 68：pend_delta_ 的 ADD 必须先于合并它的 DEL 入账 ----------
    {
        static u8 arena[256 * PAGE];
        static RamDev dev(4096);
        std::vector<u8> v(200);
        u32 runs_after = 0, bad = 0, used_before = 0, used_after = 0;
        {
            Store s; g_tr = &s;
            CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
            s.set_async_writes(true);
            s.set_io_threads(2);
            s.set_async_flush_min(16);
            for (u64 k = 1; k <= 400; ++k) { fillv(v, k, 1); s.put(k, v.data(), (u16)v.size()); }
            CHECK(s.commit() == RC::OK, "commit 1");
            for (u64 k = 401; k <= 800; ++k) { fillv(v, k, 1); s.put(k, v.data(), (u16)v.size()); }
            CHECK(s.commit(false) == RC::OK, "commit(false)");   // 后台刷出 run F，ADD 进 pend_delta_
            CHECK(s.run_count() == 2, "F 已挂上");
            CHECK(s.consolidate(0) == RC::OK, "consolidate");    // 把 F 合并掉：DEL F 直接 commit_delta
            CHECK(s.commit() == RC::OK, "commit 2");             // 旧实现在这里把过期的 ADD F 追加进清单
            used_before = s.used_pages();
            s.set_async_writes(false);               // 收线程（析构也会做；这里显式一点）
        }
        {
            Store s; g_tr = &s;
            CHECK(s.open(&dev, arena, 256) == RC::OK, "reopen");
            runs_after = s.run_count();
            used_after = s.used_pages();
            bad = readback(s, 1, 800, 1, 200);
            Report r; s.verify(&r);
            std::printf("pend_delta_ 顺序：重开 runs=%u used %u→%u 回读坏=%u bad_manifest=%u verify=%s\n",
                        runs_after, used_before, used_after, bad, s.bad_manifest(), r.errors ? r.first_err : "OK");
            CHECK(runs_after == 1, "重开后没有僵尸 run（坑 68）");
            CHECK(bad == 0 && r.errors == 0, "重开后数据完整");
        }
    }

    // ---------- 3. 坑 69：快照必须包含逻辑提交（pend_delta_ / mem_） ----------
    {
        static u8 arena[256 * PAGE];
        static RamDev dev(4096);
        Store s; g_tr = &s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
        s.set_async_writes(true);
        s.set_io_threads(2);
        s.set_async_flush_min(16);
        std::vector<u8> v(100);
        for (u64 k = 1; k <= 100; ++k) { fillv(v, k, 1); s.put(k, v.data(), (u16)v.size()); }
        CHECK(s.commit(false) == RC::OK, "commit(false) → 后台 run");
        for (u64 k = 101; k <= 105; ++k) { fillv(v, k, 1); s.put(k, v.data(), (u16)v.size()); }
        CHECK(s.commit(false) == RC::OK, "commit(false) → 留在 mem_ 里的逻辑提交");
        CHECK(s.snapshot() == RC::OK, "snapshot");
        for (u64 k = 1; k <= 105; ++k) { fillv(v, k, 9); s.put(k, v.data(), (u16)v.size()); }
        s.del(50);
        CHECK(s.commit() == RC::OK, "commit after snapshot");
        CHECK(s.rollback(0) == RC::OK, "rollback");
        u32 bad = readback(s, 1, 105, 1, 100);
        std::printf("快照含逻辑提交：回退后 1..105 回读坏=%u\n", bad);
        CHECK(bad == 0, "回退回到快照时刻的全部已提交写（坑 69）");
        // 回退之后继续工作、再提交、再回读
        for (u64 k = 1; k <= 10; ++k) { fillv(v, k, 3); s.put(k, v.data(), (u16)v.size()); }
        CHECK(s.commit() == RC::OK, "commit after rollback");
        CHECK(readback(s, 1, 10, 3, 100) == 0 && readback(s, 11, 105, 1, 100) == 0, "回退后继续写读正确");
        s.forget_snapshots();
    }

    // ---------- 4. 布局：compact_file 后清单页不能卡在低位大洞里 ----------
    {
        static u8 arena[256 * PAGE];
        static RamDev dev(4096);
        Store s; g_tr = &s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
        std::vector<u8> v(300);
        // 几轮"写一批 + 提交 + 压实"，每轮让 run 搬一次家；每轮之后占用都必须贴着存活页
        u32 worst_gap = 0;
        for (int round = 0; round < 6; ++round) {
            for (int b = 0; b < 4; ++b) {
                for (u64 k = 1 + b * 500; k <= (u64)(b + 1) * 500; ++k) { fillv(v, k, round); s.put(k, v.data(), (u16)v.size()); }
                CHECK(s.commit() == RC::OK, "commit");
            }
            CHECK(s.compact_file() == RC::OK, "compact_file");
            u32 used = s.used_pages(); u64 live = s.live_page_count();
            u32 gap = used > live ? (u32)(used - live) : 0;
            worst_gap = std::max(worst_gap, gap);
            std::printf("  round %d: used=%u live=%llu gap=%u extents=%u runs=%u\n", round, used,
                        (unsigned long long)live, gap, s.free_extents(), s.run_count());
        }
        CHECK(worst_gap <= 4, "compact_file 后 used − live ≤ 4（清单页不卡在低位洞里）");
        CHECK(readback(s, 1, 2000, 5, 300) == 0, "回读");
    }

    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
