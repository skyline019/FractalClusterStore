// c1：scrub —— 绕过页缓存、按 CRC 校验介质上的每一页。
// 单副本没有冗余可修，它的价值是"在错误传播之前发现"。
#include "fcs.h"
#include <cstdio>
#include <cstring>
using namespace fcs;
static u8 arena[256*PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)
int main(){
    static RamDev dev(4096);
    u32 pages=0, victim=0;
    { Store s; s.open(&dev, arena, 256);
      u8 v[32]; std::memset(v,'s',sizeof v);
      for (int i=0;i<20000;i++){ s.put((u64)i+1, v, 24); if(i%1000==999) s.commit(); }
      s.commit(); pages=s.used_pages();
      u32 bad=0,first=0;
      RC r0 = s.scrub(&bad,&first);
      CHECK(r0 == RC::OK && bad == 0, "干净库 scrub 应当全绿");
      printf("clean: rc=%s bad=%u\n", rc_str(r0), bad);
      // 找一个数据页翻转一位
      for (u32 p=2;p<pages;++p) if (rd32(dev.raw(p))==PK_DATA) { victim=p; break; }
      dev.raw(victim)[PAGE/2] ^= 0x40;
      RC rc = s.scrub(&bad,&first);
      CHECK(rc == RC::CORRUPT && bad == 1 && first == victim, "翻转一位必须被 scrub 抓到");
      printf("flip page %u: rc=%s bad=%u first=%u\n", victim, rc_str(rc), bad, first);
    }
    { Store s; RC orc = s.open(&dev, arena, 256); printf("reopen rc=%s\n", rc_str(orc));
      u32 bad=0,first=0;
      RC src = s.scrub(&bad,&first);
      CHECK(src == RC::CORRUPT && bad == 1 && first == victim, "重开后仍然抓到（缓存不是唯一真相）");
      printf("reopen scrub: rc=%s bad=%u first=%u\n", rc_str(src), bad, first);
      Report r; s.verify(&r);
      printf("verify errors=%u first=%s\n", r.errors, r.errors?r.first_err:"-");
      CHECK(r.errors > 0, "verify 也必须报错（两条独立路径都同意才可信）");
    }
    {   // 溢出链载荷翻一位：页头 CRC 挡不住（载荷不在数据页上），
        // 只能靠描述符里的整链 crc —— scrub / get 都必须报 CORRUPT
        const char* P2 = "build/c1b.db"; std::remove(P2);
        FileDev fd2(P2, 4000);
        u32 valpg = 0, valoff = 0;
        {
            Store s;
            CHECK(s.open(&fd2, arena, 512) == RC::OK, "ovfl open");
            std::vector<u8> v(12000, 0x6B);
            CHECK(s.put(1, v.data(), (u16)v.size()) == RC::OK, "ovfl put");
            CHECK(s.commit() == RC::OK, "ovfl commit");
            u8 b[16]; u16 l = 0;
            CHECK(s.get(1, b, sizeof b, &l) == RC::OK && l == 12000, "ovfl get");
        }
        // 找链首页：第一条 kind='OVFL' 的页
        static RamDev probe(4000);
        std::vector<u8> pg(PAGE);
        for (u32 pid = 2; pid < 200; ++pid) {
            fd2.read(pid, 0, pg.data(), PAGE);
            if (rd32(pg.data()) == 0x4C46564Fu) { valpg = pid; break; }
        }
        CHECK(valpg != 0, "找到溢出链");
        fd2.read(valpg, 0, pg.data(), PAGE);
        pg[OV_HDR + 100] ^= 0x01;                    // 载荷中间翻一位
        fd2.write(valpg, 0, pg.data(), PAGE);
        (void)valoff;
        Store s2;
        CHECK(s2.open(&fd2, arena, 512) == RC::OK, "ovfl reopen");
        u8 b[16]; u16 l = 0;
        CHECK(s2.get(1, b, sizeof b, &l) == RC::CORRUPT || s2.get(1, b, sizeof b, &l) != RC::OK,
              "载荷坏了 get 必须报错（而不是返回坏数据）");
        u32 bad = 0, first = 0;
        CHECK(s2.scrub(&bad, &first) == RC::CORRUPT && bad >= 1, "scrub 必须抓到溢出链");
        std::printf("溢出链载荷翻位：scrub bad=%u first=%u\n", bad, first);
    }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
