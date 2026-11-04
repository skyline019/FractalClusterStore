// c3：坑 125 — Soft 近满加速（space_accel）：护洞已开时，eff_tail 逼近 user_lim
// 视同维护积压；消融 set_space_accel_pct(0) 可关。正确性：小盘覆盖写后仍可读且不脏。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;
static u8 arena[512 * PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}

// 小盘高压：大值 + 多键直到 NOSPACE 或写满轮次；返回观测
static void run_arm(u32 pages, u32 accel_pct, u32& nospace, u64& accels, u32& peak, u32& ok_gets) {
    RamDev dev(pages);
    Store s;
    CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
    s.configure_deploy(false, 150);
    s.set_space_accel_pct(accel_pct);   // 覆盖 Soft 默认，便于定点触发 / 消融
    CHECK(s.protect_merge_holes(), "护洞开");
    CHECK(s.space_accel_pct() == accel_pct, "accel 旋钮");

    const u32 VSZ = 200;
    std::vector<u8> val(VSZ, 7), val2(VSZ, 9);
    nospace = 0; peak = 0; ok_gets = 0;
    const u32 N = 8000;
    const u32 B = 20;
    for (u32 i = 0; i < N; ++i) {
        u64 k = mix(i + 1) % (N * 2) + 1;
        RC pr = s.put(k, val.data(), (u16)VSZ);
        if (pr == RC::NOSPACE) {
            RC cr = s.commit();
            if (cr == RC::NOSPACE) ++nospace;
            pr = s.put(k, val.data(), (u16)VSZ);
            if (pr == RC::NOSPACE) { ++nospace; break; }
        }
        if ((i + 1) % B == 0) {
            RC cr = s.commit();
            if (cr == RC::NOSPACE) ++nospace;
        }
        if (s.used_pages() > peak) peak = s.used_pages();
        if (nospace > 8) break;
    }
    for (u32 i = 0; i < 400; ++i) {
        u64 k = mix(i + 999) % (N * 2) + 1;
        (void)s.put(k, val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) {
            RC cr = s.commit();
            if (cr == RC::NOSPACE) ++nospace;
        }
        if (s.used_pages() > peak) peak = s.used_pages();
    }
    (void)s.commit();
    accels = s.space_accels();
    u8 buf[256];
    u16 n = 0;
    for (u32 i = 0; i < 64; ++i) {
        u64 k = mix(i + 1) % (N * 2) + 1;
        if (s.get(k, buf, sizeof buf, &n) == RC::OK) ++ok_gets;
    }
    std::printf("  arm pages=%u accel_pct=%u nospace=%u accels=%llu peak=%u gets=%u\n",
                pages, accel_pct, nospace, (unsigned long long)accels, peak, ok_gets);
}

int main() {
    CHECK(FCS_SPACE_ACCEL_PCT >= 50 && FCS_SPACE_ACCEL_PCT <= 95, "default pct sane");
    {
        // Soft 默认：护洞开、space_accel 关（harness 消融无峰增益，坑 125）
        RamDev d(256);
        Store s;
        CHECK(s.open(&d, arena, 64) == RC::OK, "open soft");
        s.configure_deploy(true, 150);
        CHECK(s.space_accel_pct() == 0, "Soft 默认不开 space_accel");
        CHECK(s.protect_merge_holes(), "Soft 护洞");
    }

    u32 ns0 = 0, ns1 = 0, pk0 = 0, pk1 = 0, g0 = 0, g1 = 0;
    u64 a0 = 0, a1 = 0;
    // 128 页 + 低阈值 50%：必顶近满（user_lim≈110，50%≈55；大值写入 peak 通常更高）
    run_arm(128, 50, ns1, a1, pk1, g1);
    CHECK(g1 > 0, "accel 臂可读到键");
    CHECK(a1 > 0, "近满应触发 space_accels");

    // 消融
    run_arm(128, 0, ns0, a0, pk0, g0);
    CHECK(g0 > 0, "消融臂可读到键");
    CHECK(a0 == 0, "消融臂 space_accels 应为 0");

    // A2 三档 + Soft 默认 pct：正确性冒烟（峰数字留给 harness）
    for (u32 pg : {256u, 512u, 1024u}) {
        u32 ns = 0, pk = 0, g = 0; u64 a = 0;
        run_arm(pg, FCS_SPACE_ACCEL_PCT, ns, a, pk, g);
        CHECK(g > 0, "A2 档可读");
    }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
