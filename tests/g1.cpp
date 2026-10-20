// g1：范围墓碑 del_range(lo,hi)。
// 墓碑只活在清单里（M_RANGE），不进键空间 ⇒ 扫描不需要特殊处理。
// 验证：删中段 / 边界值 / 删后再写（新版本要可见）/ 重开恢复 / 归一物化后墓碑清空。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;
static u8 arena[512 * PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)
static int count(Store& s, u64 lo, u64 hi) {
    int n = 0;
    s.select(lo, hi, [&](u64, const void*, u16) { ++n; return true; });
    return n;
}
int main() {
    const char* P = "build/g1.db"; std::remove(P);
    {
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open");
        u8 v[16]; std::memset(v, 1, sizeof v);
        for (u64 k = 1; k <= 10000; ++k) {
            s.put(k, v, 12);
            if (k % 1000 == 0) s.commit();
        }
        s.commit();
        CHECK(count(s, 0, ~0ull) == 10000, "全部可见");
        // 删中段 3000 个
        CHECK(s.del_range(4001, 7001) == RC::OK, "del_range");
        CHECK(s.commit() == RC::OK, "commit");
        CHECK(count(s, 0, ~0ull) == 7000, "删掉 3000 个");
        u8 b[16]; u16 l = 0;
        CHECK(s.get(5000, b, sizeof b, &l) == RC::NOTFOUND, "get 被删");
        CHECK(s.get(3000, b, sizeof b, &l) == RC::OK, "get 边界外");
        CHECK(s.get(7001, b, sizeof b, &l) == RC::OK, "get 右边界（开区间）");
        // 删了之后再写进去的必须可见（版本更新）
        u8 v2[16]; std::memset(v2, 9, sizeof v2);
        s.put(5555, v2, 12);
        s.commit();
        CHECK(s.get(5555, b, sizeof b, &l) == RC::OK && b[0] == 9, "删后再写应可见");
        CHECK(count(s, 0, ~0ull) == 7001, "删后再写 +1");
        std::printf("写/删/再写：%s\n", fails ? "有失败" : "OK");
    }
    {   // 重开：墓碑必须从清单恢复
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "reopen");
        CHECK(s.range_tombstones() == 1, "重开后墓碑数");
        CHECK(count(s, 0, ~0ull) == 7001, "重开后可见数");
        u8 b[16]; u16 l = 0;
        CHECK(s.get(5000, b, sizeof b, &l) == RC::NOTFOUND, "重开后 get 被删");
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "verify");
        std::printf("重开：%s (runs=%u)\n", fails ? "有失败" : "OK", s.run_count());
    }
    {   // 回退：墓碑属于清单状态，回退到"删之前"必须复原
        const char* P3 = "build/g1b.db"; std::remove(P3);
        FileDev fd(P3, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "rollback open");
        u8 v[16]; std::memset(v, 3, sizeof v);
        for (u64 k = 1; k <= 500; ++k) s.put(k, v, 12);
        s.commit();
        CHECK(s.snapshot() == RC::OK, "rollback snapshot");
        CHECK(count(s, 0, ~0ull) == 500, "删前 500");
        CHECK(s.del_range(101, 401) == RC::OK, "rollback del_range");
        CHECK(s.commit() == RC::OK, "rollback commit");
        CHECK(count(s, 0, ~0ull) == 200, "删后 200");
        CHECK(s.rollback(0) == RC::OK, "rollback");
        CHECK(s.range_tombstones() == 0, "回退后墓碑必须清掉");
        CHECK(count(s, 0, ~0ull) == 500, "回退后 500 条全部回来");
        // 回退之后新写的范围删除仍然要生效（内存状态和清单不能各说各话）
        CHECK(s.del_range(1, 51) == RC::OK, "回退后 del_range");
        CHECK(s.commit() == RC::OK, "回退后 commit");
        CHECK(count(s, 0, ~0ull) == 450, "回退后新墓碑生效");
        std::printf("回退+墓碑：%s\n", fails ? "有失败" : "OK");
    }
    {   // 归一：覆盖的数据真的丢掉，墓碑随之清空
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open3");
        u32 before = s.used_pages();
        CHECK(s.consolidate(0) == RC::OK, "consolidate");
        CHECK(s.range_tombstones() == 0, "归一后墓碑应清空");
        CHECK(count(s, 0, ~0ull) == 7001, "归一后可见数");
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "归一 verify");
        std::printf("归一：used %u -> %u，%s\n", before, s.used_pages(), fails ? "有失败" : "OK");
    }
    {   // 再重开一次，墓碑不能"复活"
        FileDev fd(P, 20000);
        Store s;
        CHECK(s.open(&fd, arena, 512) == RC::OK, "open4");
        CHECK(s.range_tombstones() == 0, "重开后墓碑仍应为 0");
        CHECK(count(s, 0, ~0ull) == 7001, "重开后可见数（墓碑已物化）");
        CHECK(s.get(5000, (void*)nullptr, 0, nullptr) == RC::NOTFOUND, "数据真的没了");
        std::printf("归一后重开：%s\n", fails ? "有失败" : "OK");
    }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
