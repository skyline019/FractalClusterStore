// s4：多版本快照的**正确性**+ 快照感知的页复用。
//
// 两件事：
//  1) 回收判据必须是 snap_max（最新快照），不是 snap_min（最老快照）：
//     视图 V 看见的是 ver <= V 的记录。取两个快照 A(V0)、B(V1) 之后继续覆盖，
//     B 必须仍然读到 gen1、A 必须仍然读到 gen0。用 snap_min 当门槛会把
//     "ver 落在 (V0, V1] 的 run" 压掉，B 的视图直接丢记录。
//  2) 只有 ver > snap_max 的 run 才**推迟释放**；更年轻的 run 死了就能立刻复用页。
//     这一条决定"长快照下文件会不会只涨不落"。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;
static u8 arena[512 * PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

int main() {
    const char* P = "build/s4.db"; std::remove(P);
    FileDev fd(P, 20000);
    Store s;
    RC orc = s.open(&fd, arena, 512);
    if (orc != RC::OK) { std::printf("open rc=%s\n", rc_str(orc)); return 1; }

    const int N = 2000;
    auto putgen = [&](int g) {
        std::vector<u8> v(40);
        for (int i = 0; i < N; ++i) { std::memset(v.data(), g, v.size()); s.put((u64)i + 1, v.data(), 40); }
        s.commit();
    };
    putgen(0);
    CHECK(s.snapshot() == RC::OK, "snap A");
    const u32 vA = s.snap_version(0);
    putgen(1);
    CHECK(s.snapshot() == RC::OK, "snap B");
    const u32 vB = s.snap_version(1);
    std::printf("V(A)=%u V(B)=%u\n", vA, vB);

    u32 used_before = s.used_pages();
    for (int g = 2; g <= 12; ++g) putgen(g);          // 期间会触发压实/回收
    std::printf("覆盖 11 轮后：used %u → %u，runs=%u\n", used_before, s.used_pages(), s.run_count());

    auto check_view = [&](u32 view, int want, const char* tag) {
        int n = 0, bad = 0;
        s.select_at(view, 0, ~0ull, [&](u64, const void* p, u16 l) {
            ++n;
            if (l != 40 || ((const u8*)p)[0] != (u8)want) ++bad;
            return true;
        });
        std::printf("  %s: n=%d bad=%d\n", tag, n, bad);
        if (n != N || bad) { std::printf("  FAIL %s 视图\n", tag); ++fails; }
    };
    check_view(vA, 0, "A(gen0)");
    check_view(vB, 1, "B(gen1)");
    check_view(0xFFFFFFFFu, 12, "当前(gen12)");

    // 长快照下空间必须有界：新 run 死掉就该复用，不能一直涨
    Report r; s.verify(&r);
    CHECK(r.errors == 0, "verify");
    u64 live = s.live_page_count();
    std::printf("used=%u live=%llu ratio=%.2f\n", s.used_pages(), (unsigned long long)live,
                live ? (double)s.used_pages() / live : 0.0);
    CHECK(s.used_pages() <= (u32)(live * 2 + 32), "长快照下空间必须有界（≤2×活页+32）");

    // 回退到 B：当前视图变 gen1，A 仍然要能读
    CHECK(s.rollback(1) == RC::OK, "rollback(B)");
    check_view(0xFFFFFFFFu, 1, "回退后当前(gen1)");
    check_view(vA, 0, "回退后 A(gen0)");
    Report r2; s.verify(&r2);
    CHECK(r2.errors == 0, "回退后 verify");

    // 释放快照：延迟页归位，再写两轮空间仍然有界
    CHECK(s.forget_snapshots() == RC::OK, "forget");
    for (int g = 20; g <= 22; ++g) putgen(g);
    Report r3; s.verify(&r3);
    CHECK(r3.errors == 0, "forget 后 verify");
    u64 live2 = s.live_page_count();
    std::printf("forget 后：used=%u live=%llu runs=%u\n", s.used_pages(),
                (unsigned long long)live2, s.run_count());
    // used 是**高水位**：快照释放只是把页放回空闲表，不会自己把文件缩短。
    // 真正"还给文件系统"要靠 compact_file()（归一+搬家+回落）。
    CHECK(s.compact_file() == RC::OK, "compact_file");
    u64 live3 = s.live_page_count();
    std::printf("compact_file 后：used=%u live=%llu runs=%u\n", s.used_pages(),
                (unsigned long long)live3, s.run_count());
    CHECK(s.used_pages() <= (u32)(live3 * 2 + 32), "紧凑之后高水位必须贴近活页");

    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
