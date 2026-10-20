// s3：开着快照做覆盖写，回收必须继续推进（以前 nsnap_>0 会全面停摆）。
// 只回收 ver > snap_max() 的 run，所以老快照读到的必须始终是它那一刻的值。
// 同时断言**空间有界**：这些"比所有快照都新"的 run 死了以后页是立刻复用的，
// 不再像第一版那样全部塞进 deferred_（曾经 44 → 620 页，14.1×）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
using namespace fcs;
static u8 arena[512 * PAGE];
int main() {
    static RamDev dev(20000);
    Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { printf("open fail\n"); return 1; }
    const int K = 3000, R = 30;
    u8 v[24];
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < K; ++i) {
            std::memset(v, (u8)('a' + r), sizeof v);
            s.put((u64)i + 1, v, 20);
        }
        s.commit();
    }
    u32 snap_id = 0;
    if (s.snapshot() != RC::OK) { printf("snapshot fail\n"); return 1; }
    u32 used0 = s.used_pages();
    u32 max_used = used0;
    for (int r = 2; r < R; ++r) {
        for (int i = 0; i < K; ++i) {
            std::memset(v, (u8)('a' + r), sizeof v);
            s.put((u64)i + 1, v, 20);
        }
        s.commit();
        if (s.used_pages() > max_used) max_used = s.used_pages();
    }
    // 空间必须有界：快照存活时 used ≤ 2×活页 + 32
    {
        u64 live = s.live_page_count();
        std::printf("快照期间 used=%u live=%llu ratio=%.2f（峰值 %u）\n",
                    s.used_pages(), (unsigned long long)live,
                    live ? (double)s.used_pages() / live : 0.0, max_used);
        if (s.used_pages() > (u32)(live * 2 + 32)) {
            std::printf("  FAIL 快照期间空间失控\n");
            return 1;
        }
    }
    // 老快照必须还是第 1 轮写的值
    int bad_snap = 0;
    s.select_at(s.snap_version(0), 0, ~0ull, [&](u64, const void* p, u16 l) {
        if (l != 20 || ((const u8*)p)[0] != 'b') ++bad_snap;
        return true;
    });
    printf("快照期间：used %u -> %u（峰值 %u），%.1f 倍；老快照读错 %d 条\n",
           used0, s.used_pages(), max_used, (double)max_used / used0, bad_snap);
    // 当前视图
    int bad_now = 0;
    s.select(0, ~0ull, [&](u64, const void* p, u16 l) {
        if (l != 20 || ((const u8*)p)[0] != (u8)('a' + R - 1)) ++bad_now;
        return true;
    });
    printf("当前视图读错 %d 条\n", bad_now);
    Report r; s.verify(&r);
    printf("verify errors=%u runs=%u\n", r.errors, s.run_count());
    s.forget_snapshots();
    s.commit();
    printf("释放快照后 used=%u runs=%u\n", s.used_pages(), s.run_count());
    int fails = (bad_snap || bad_now || r.errors) ? 1 : 0;
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails;
}
