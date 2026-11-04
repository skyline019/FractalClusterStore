// c4：坑 132/133 — protect_stream_small = 流式 anti-shatter（勿切 (0,floor) 残桩）；Soft 默认关。
#define private public
#include "fcs.h"
#undef private
#include <cstdio>
using namespace fcs;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

static bool remake_mid_hole(Store& s, u32& hole_pg, u32 hole_np, u32 spacer = 64) {
    u32 a = 0, b = 0;
    if (s.alloc_pages(hole_np, a, true) != RC::OK) { CHECK(false, "maint hole"); return false; }
    if (s.alloc_pages(spacer, b, true) != RC::OK) { CHECK(false, "maint spacer"); return false; }
    s.free_[a] = hole_np;
    s.coalesce();
    hole_pg = a;
    return true;
}

static Store* open_softish(RamDev& dev, u8* arena, u32 pages) {
    auto* s = new Store;
    if (s->open(&dev, arena, pages) != RC::OK) { CHECK(false, "open"); delete s; return nullptr; }
    s->set_protect_merge_holes(true);
    s->set_hole_protect_pages(64);
    return s;
}

int main() {
    // --- 默认：stream 可切大洞（与 g18 一致）---
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store* s = open_softish(dev, arena, 512);
        if (!s) return 1;
        CHECK(!s->protect_stream_small(), "P1 默认关");

        u32 hole = 0, c = 0;
        remake_mid_hole(*s, hole, 128);
        s->stream_alloc_ = true;
        CHECK(s->alloc_pages(32, c, false) == RC::OK, "stream 32 default");
        s->stream_alloc_ = false;
        CHECK(c == hole, "默认 stream 可吃中部洞");
        delete s;
    }

    // --- P1b：残桩 ≥ floor ⇒ 允许 ---
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store* s = open_softish(dev, arena, 512);
        if (!s) return 1;
        s->set_protect_stream_small(true);

        u32 hole = 0, c = 0;
        remake_mid_hole(*s, hole, 128);
        s->stream_alloc_ = true;
        CHECK(s->alloc_pages(32, c, false) == RC::OK, "stream 32 leave≥floor");
        s->stream_alloc_ = false;
        CHECK(c == hole, "P1b：残桩≥floor 可切");
        delete s;
    }

    // --- P1b：残桩 < floor ⇒ 跳过 ---
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store* s = open_softish(dev, arena, 512);
        if (!s) return 1;
        s->set_protect_stream_small(true);

        u32 hole = 0, c = 0;
        remake_mid_hole(*s, hole, 80);
        const u64 sk0 = s->hole_protect_skips();
        s->stream_alloc_ = true;
        CHECK(s->alloc_pages(32, c, false) == RC::OK, "stream 32 shatter");
        s->stream_alloc_ = false;
        CHECK(c != hole, "坑133：残桩<floor 不得切碎");
        CHECK(s->hole_protect_skips() > sk0, "skip 计数增加");
        delete s;
    }

    // --- P1b：整吃允许 ---
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store* s = open_softish(dev, arena, 512);
        if (!s) return 1;
        s->set_protect_stream_small(true);

        u32 hole = 0, c = 0;
        remake_mid_hole(*s, hole, 80);
        s->stream_alloc_ = true;
        CHECK(s->alloc_pages(80, c, false) == RC::OK, "stream 80 whole");
        s->stream_alloc_ = false;
        CHECK(c == hole, "整吃大洞允许");
        delete s;
    }

    // --- Soft 默认不开 ---
    {
        RamDev d2(1024);
        static u8 arena2[64 * PAGE];
        Store s2;
        CHECK(s2.open(&d2, arena2, 64) == RC::OK, "soft open");
        s2.configure_deploy(false, 150);
        CHECK(s2.protect_merge_holes(), "Soft 护洞");
        CHECK(!s2.protect_stream_small(), "Soft 默认不开 protect_stream_small");
    }

    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
