// soak1：坑 121 — 短/长时混合写 + 周期 verify + 偶发 scrub + 重开。
// 环境变量 FCS_SOAK_SEC（默认 30）；make soak-smoke 用短时，长跑人工设 3600+。
// 本测试不是真拔电矩阵（见 f3/g6/h3）。
#include "fcs.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace fcs;

static u8 arena[512 * PAGE];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL %s\n", m); ++fails; } } while (0)

static u64 now_sec() {
    return (u64)std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int main(int argc, char** argv) {
    u64 budget = 30;
    if (const char* e = std::getenv("FCS_SOAK_SEC")) budget = (u64)std::strtoull(e, nullptr, 10);
    if (argc > 1) budget = (u64)std::strtoull(argv[1], nullptr, 10);
    if (budget < 5) budget = 5;

    const char* path = "build/soak1.db";
    std::remove(path);
    FileDev fd(path, 1u << 20);   // 大上限；物理按需涨
    Store s;
    CHECK(s.open(&fd, arena, 512) == RC::OK, "open");
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.set_verify_reads(true);     // soak 打开读 CRC：尽早抓静默坏页

    const u64 t0 = now_sec();
    u64 ops = 0, commits = 0, verifies = 0;
    u8 val[64];
    std::memset(val, 0xA5, sizeof val);
    u64 seed = 0xC0FFEE;
    auto rnd = [&]() {
        seed = seed * 6364136223846793005ull + 1;
        return seed;
    };

    while (now_sec() - t0 < budget) {
        const u64 r = rnd();
        const u64 k = (r % 50000ull) + 1;
        const int kind = (int)(r % 7);
        if (kind < 4) {
            val[0] = (u8)(r >> 8);
            CHECK(s.put(k, val, 32) == RC::OK || s.put(k, val, 32) == RC::NOSPACE, "put");
        } else if (kind == 4) {
            s.del(k);
        } else if (kind == 5) {
            u64 lo = k, hi = k + 50;
            if (hi > 50001) hi = 50001;
            s.del_range(lo, hi);
        } else {
            CHECK(s.commit() == RC::OK || s.commit() == RC::NOSPACE, "commit");
            ++commits;
        }
        ++ops;
        if ((ops % 2000) == 0) {
            CHECK(s.commit() == RC::OK || s.commit() == RC::NOSPACE, "batch commit");
            ++commits;
            Report rep;
            RC vr = s.verify(&rep);
            CHECK(vr == RC::OK && rep.errors == 0, "verify clean");
            ++verifies;
            if ((verifies % 3) == 0) {
                u32 bad = 0, first = 0;
                RC sr = s.scrub(&bad, &first, 4ull << 20);
                CHECK(sr == RC::OK || sr == RC::CORRUPT, "scrub rc");
                if (sr == RC::CORRUPT) {
                    std::printf("  FAIL scrub found bad=%u first=%u\n", bad, first);
                    ++fails;
                }
            }
            if ((verifies % 5) == 0) {
                CHECK(s.open(&fd, arena, 512) == RC::OK, "reopen");
                s.set_tidy_policy(Store::TidyPolicy::Soft);
                s.set_verify_reads(true);
            }
        }
    }
    CHECK(s.commit() == RC::OK || s.commit() == RC::NOSPACE, "final commit");
    Report rep;
    CHECK(s.verify(&rep) == RC::OK && rep.errors == 0, "final verify");
    std::printf("soak1 sec=%llu ops=%llu commits=%llu verifies=%llu runs=%u\n",
                (unsigned long long)budget, (unsigned long long)ops, (unsigned long long)commits,
                (unsigned long long)verifies, s.run_count());
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
