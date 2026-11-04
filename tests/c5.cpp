// c5：坑 134/135 — stream_best_fit 模式：全量 / 仅小 np；Soft 默认关。
#define private public
#include "fcs.h"
#undef private
#include <cstdio>
using namespace fcs;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

static void plant_holes(Store& s, u32& big, u32& small) {
    u32 spacer = 0;
    CHECK(s.alloc_pages(128, big, true) == RC::OK, "big");
    CHECK(s.alloc_pages(32, spacer, true) == RC::OK, "sp1");
    CHECK(s.alloc_pages(40, small, true) == RC::OK, "small");
    CHECK(s.alloc_pages(32, spacer, true) == RC::OK, "sp2");
    s.free_[big] = 128;
    s.free_[small] = 40;
    s.coalesce();
}

int main() {
    // mode1 全量：小分配吃最小洞
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open1");
        s.set_protect_merge_holes(true);
        s.set_hole_protect_pages(64);
        s.set_stream_best_fit_mode(1);
        u32 big = 0, small = 0, c = 0;
        plant_holes(s, big, small);
        s.stream_alloc_ = true;
        CHECK(s.alloc_pages(32, c, false) == RC::OK, "m1 stream32");
        s.stream_alloc_ = false;
        CHECK(c == small, "mode1：小分配吃最小洞");
    }

    // mode2 仅小 np：32 吃小洞；80≥floor 仍 first-fit 吃大洞
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open2");
        s.set_protect_merge_holes(true);
        s.set_hole_protect_pages(64);
        s.set_stream_best_fit_mode(2);
        u32 big = 0, small = 0, c = 0;
        plant_holes(s, big, small);
        s.stream_alloc_ = true;
        CHECK(s.alloc_pages(32, c, false) == RC::OK, "m2 stream32");
        s.stream_alloc_ = false;
        CHECK(c == small, "mode2：小 np 仍 best-fit");

        plant_holes(s, big, small);
        s.stream_alloc_ = true;
        CHECK(s.alloc_pages(80, c, false) == RC::OK, "m2 stream80");
        s.stream_alloc_ = false;
        CHECK(c == big, "mode2：大 np first-fit 低址大洞");
    }

    // exact 早停：仅一个恰好 32 的洞 + 更大洞，取 32
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open3");
        s.set_stream_best_fit_mode(1);
        u32 a = 0, b = 0, sp = 0, c = 0;
        CHECK(s.alloc_pages(100, a, true) == RC::OK, "a");
        CHECK(s.alloc_pages(16, sp, true) == RC::OK, "sp");
        CHECK(s.alloc_pages(32, b, true) == RC::OK, "b");
        CHECK(s.alloc_pages(16, sp, true) == RC::OK, "sp2");
        s.free_[a] = 100;
        s.free_[b] = 32;
        s.coalesce();
        s.stream_alloc_ = true;
        CHECK(s.alloc_pages(32, c, false) == RC::OK, "exact32");
        s.stream_alloc_ = false;
        CHECK(c == b, "exact 早停取 32 洞");
    }

    // Soft 默认关
    {
        RamDev d2(1024);
        static u8 arena2[64 * PAGE];
        Store s2;
        CHECK(s2.open(&d2, arena2, 64) == RC::OK, "soft");
        s2.configure_deploy(false, 150);
        CHECK(s2.stream_best_fit_mode() == 0, "Soft 默认 mode0");
        CHECK(s2.stream_best_fit_skip_pct() == 0, "Soft 默认无 skip_pct");
    }

    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
