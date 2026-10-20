#include "store.h"
#include <cstdio>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
static void raw_check(RamDev& dev, const char* tag) {
    u32 bad = 0;
    for (u32 p = 2; p < dev.capacity(); ++p) {
        const u8* b = dev.raw((PageId)p);
        bool zero = true;
        for (u32 i = 0; i < 32 && zero; ++i) if (b[i]) zero = false;
        if (zero) continue;
        if (!page_ok(b)) { if (bad < 5) std::printf("   %s: page %u hash bad kind=%u\n", tag, p, b[0]); ++bad; }
    }
    if (bad) std::printf("  %s: %u page-hash failures\n", tag, bad);
}
int main(int argc, char** argv) {
    u32 nins = argc>1?atoi(argv[1]):6000;
    u32 cint = argc>2?atoi(argv[2]):100;
    RamDev dev(4096);
    Engine e;
    RC rc = e.open(&dev, arena, NAV_CAP);
    if (rc != RC::OK) { std::printf("open %s\n", rc_str(rc)); return 1; }
    u32 expect = 0;
    for (u32 i = 1; i <= nins; ++i) {
        u32 v = i * 3;
        rc = e.put(i, &v, 4);
        if (rc != RC::OK) { std::printf("put %u rc=%s\n", i, rc_str(rc)); break; }
        expect = i;
        if (i % cint == 0) {
            rc = e.commit();
            if (rc != RC::OK) { std::printf("commit rc=%s at %u\n", rc_str(rc), i); return 1; }
            u32 cnt = 0;
            e.scan([&](u64, const u8*, u16) { ++cnt; return true; });
            if (cnt != expect) {
                std::printf("LOST at i=%u: scan=%u expect=%u\n", i, cnt, expect);
                Engine::VerifyReport r;
                e.verify(&r);
                std::printf("  verify err=%u first=%u(%s) leaves=%u inners=%u\n", r.errors, r.first_err_page, r.first_err, r.leaves, r.inners);
                raw_check(dev, "commit");
                return 1;
            }
        }
    }
    std::printf("insert-only ok, pages used=%u\n", e.free_count());
    return 0;
}
