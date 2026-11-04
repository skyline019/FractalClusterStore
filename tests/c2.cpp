// c2：坑 120 — set_verify_reads：数据页 CRC 在 page() 装填时校验。
// 在**仍被 run 引用**的活页上翻 CRC 覆盖区内字节；开启校验须清缓存后再 get。
#include "fcs.h"
#include <cstdio>
#include <cstring>
using namespace fcs;
static u8 arena[256 * PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

int main() {
    static RamDev dev(4096);
    {
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
        CHECK(!s.verify_reads(), "默认 verify_reads 关闭");
        u8 v[32];
        std::memset(v, 'C', sizeof v);
        // 单次提交：键连续，便于按 first/last 定位活页（避免扫到已退役 PK_DATA 残骸）
        for (int i = 1; i <= 2000; ++i) CHECK(s.put((u64)i, v, 24) == RC::OK, "put");
        CHECK(s.commit() == RC::OK, "commit");
        const u64 hit_key = 1;
        u8 buf[64];
        u16 n = 0;
        CHECK(s.get(hit_key, buf, sizeof buf, &n) == RC::OK && n == 24, "翻转前 get OK");

        u32 victim = 0;
        u16 used = 0;
        for (u32 p = 2; p < s.used_pages(); ++p) {
            if (rd32(dev.raw(p)) != PK_DATA) continue;
            u64 first = rd64(dev.raw(p) + 16), last = rd64(dev.raw(p) + 24);
            if (first <= hit_key && hit_key <= last) {
                victim = p;
                used = rd16(dev.raw(p) + 34);
                break;
            }
        }
        CHECK(victim != 0 && used > DHDR + 8, "定位含 key=1 的活数据页");
        // 翻 CRC 覆盖区（[12,used)）
        dev.raw(victim)[DHDR + 4] ^= 0x01;
        CHECK(crc32c(dev.raw(victim) + 12, (size_t)(used - 12)) != rd32(dev.raw(victim) + 8), "翻转后 CRC 必失配");

        s.set_verify_reads(true);   // 清缓存 + 开校验
        CHECK(s.verify_reads(), "已打开 verify_reads");
        RC gr = s.get(hit_key, buf, sizeof buf, &n);
        CHECK(gr != RC::OK, "坑120：坏活页 get 不得 OK");
        std::printf("verify_reads get after flip: rc=%s victim=%u\n", rc_str(gr), victim);

        u32 bad = 0, first = 0;
        RC sr = s.scrub(&bad, &first);
        CHECK(sr == RC::CORRUPT && bad >= 1, "scrub 抓到坏页");
        std::printf("scrub: bad=%u first=%u\n", bad, first);
    }
    {
        static RamDev clean(4096);
        Store s;
        CHECK(s.open(&clean, arena, 256) == RC::OK, "clean open");
        s.set_verify_reads(true);
        const char* msg = "ok";
        CHECK(s.put(42, msg, 2) == RC::OK && s.commit() == RC::OK, "clean put");
        u8 buf[8];
        u16 n = 0;
        CHECK(s.get(42, buf, sizeof buf, &n) == RC::OK && n == 2, "clean get with verify_reads");
    }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
