// c6：坑 138/144 — P2 starve_fit 紧余量；Soft configure_deploy 默认开 P2。
#define private public
#include "fcs.h"
#undef private
#include <cstdio>
using namespace fcs;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

int main() {
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open0");
        CHECK(!s.part_starve_fit() && !s.starve_promote(), "member default off");
        s.configure_deploy(false, 150);
        CHECK(s.part_starve_fit() && s.starve_promote(), "Soft P2 on (pit 144)");
        CHECK(Store::part_cap_of(16) == 56, "loose cap(16)=56");
        CHECK(Store::part_cap_tight(32) == 40, "tight cap(32)=40");
        CHECK(Store::part_cap_tight(16) == 24, "tight cap(16)=24");
    }

    // 人造 40 页洞：松路径装不下 frag_floor；紧路径 choose_part 应出片
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open1");
        s.set_part_frag(true);
        s.set_part_frag_floor(16);
        s.set_part_starve_fit(true);
        u32 hole = 0, sp = 0;
        CHECK(s.alloc_pages(40, hole, true) == RC::OK, "hole40");
        CHECK(s.alloc_pages(8, sp, true) == RC::OK, "spacer");
        s.free_[hole] = 40;
        s.coalesce();
        CHECK(s.largest_hole() == 40, "maxh=40");
        CHECK(s.largest_hole() < Store::part_cap_of(16), "40 < loose(16)");
        CHECK(s.largest_hole() >= Store::part_cap_tight(16), "40 >= tight(16)");

        Store::MergeJob j;
        j.part_lo = 64;
        j.part_hi = 256;
        s.choose_part(j);
        // 坑 139：STARVE_FLOOR=32 ⇒ maxh=40 时紧切 p=32（不再落到 16）
        CHECK(j.part >= 32 && j.part <= 40, "starve choose_part in [32,40]");
        CHECK(s.part_starve_fits() >= 1, "starve_fits++");
        CHECK(j.starve_tight, "starve_tight set");
        CHECK(s.part_cap(j) <= 40, "reserve need fits hole");
        CHECK(Store::starve_part_floor(16) == 32, "starve_part_floor(16)=32");
    }

    // 关 starve_fit：同样洞 choose_part 退回 part_lo（长尾）
    {
        RamDev dev(8192);
        static u8 arena[512 * PAGE];
        Store s;
        CHECK(s.open(&dev, arena, 512) == RC::OK, "open2");
        s.set_part_frag(true);
        s.set_part_frag_floor(16);
        s.set_part_starve_fit(false);
        u32 hole = 0, sp = 0;
        CHECK(s.alloc_pages(40, hole, true) == RC::OK, "hole40b");
        CHECK(s.alloc_pages(8, sp, true) == RC::OK, "spb");
        s.free_[hole] = 40;
        s.coalesce();
        Store::MergeJob j;
        j.part_lo = 64;
        j.part_hi = 256;
        s.choose_part(j);
        CHECK(j.part == 64, "off: clamp to part_lo (tail path)");
        CHECK(s.part_starve_fits() == 0, "off: no starve_fits");
    }

    if (fails) { std::printf("c6 FAIL %d\n", fails); return 1; }
    std::printf("c6 PASS (pit 138/144 starve_fit Soft-on)\n");
    return 0;
}
