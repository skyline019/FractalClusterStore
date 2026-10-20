// tests/golden/gen.cpp —— 生成 golden 镜像（第 11 轮 H2）。**只在格式有意升版时重新生成**（P2 轮），平时不跑。
//   g++ -std=c++17 -O2 -pthread -Iinclude tests/golden/gen.cpp -o /tmp/golden_gen && /tmp/golden_gen tests/golden
// 三个小库：small（小值 + 覆盖 + 点删）、bigval（溢出链 + 覆盖链）、tomb_snap（范围墓碑 + 点墓碑 + 存活快照 + 归一）。
// 每个库输出 X.img（RamDev 前 used_pages 页的原始字节）和 X.kv（期望：`k len fnv64` 行；`@snap <vers>` 之后是快照视图）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace fcs;

static u64 fnv(const u8* p, size_t n) { u64 h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static void dump(Store& s, FILE* kv, Vers view) {
    static u8 out[70000]; u16 len = 0;
    for (u64 k = 1; k <= 6000; ++k) {
        RC rc = view ? s.get_at(k, view, out, 65535, &len) : s.get(k, out, 65535, &len);
        if (rc == RC::OK) std::fprintf(kv, "%llu %u %llu\n", (unsigned long long)k, len, (unsigned long long)fnv(out, len));
    }
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/golden";
    for (int which = 0; which < 3; ++which) {
        const char* name = which == 0 ? "small" : which == 1 ? "bigval" : "tomb_snap";
        static u8 arena[256 * PAGE];
        RamDev dev(4096);
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("open fail\n"); return 1; }
        std::mt19937_64 rng(1000 + which);
        auto put = [&](u64 k, u32 tag, u32 len) {
            std::vector<u8> v(len); for (u32 i = 0; i < len; ++i) v[i] = (u8)(tag * 31 + i * 7 + k);
            s.put(k, v.data(), (u16)len);
        };
        Vers snapv = 0;
        if (which == 0) {
            for (int c = 0; c < 6; ++c) { for (int i = 0; i < 150; ++i) put(1 + rng() % 1200, 10 + c, 8 + rng() % 120); if (c == 3) for (u64 k = 50; k < 80; ++k) s.del(k); s.commit(); }
        } else if (which == 1) {
            for (int c = 0; c < 4; ++c) { for (int i = 0; i < 6; ++i) put(2000 + i, 50 + c, 3000 + (u32)(rng() % 30000)); for (int i = 0; i < 40; ++i) put(1 + rng() % 300, 60 + c, 30); s.commit(); }
            put(2000, 99, 12345); s.commit();
        } else {
            for (int c = 0; c < 4; ++c) { for (int i = 0; i < 200; ++i) put(1 + rng() % 2000, 70 + c, 20 + rng() % 60); s.commit(); }
            s.snapshot(); snapv = s.last_commit();
            s.del_range(500, 700);
            for (u64 k = 900; k < 950; ++k) s.del(k);
            s.commit();
            for (int i = 0; i < 100; ++i) put(1 + rng() % 2000, 80, 40);
            s.commit();
            s.consolidate();
        }
        std::string img = dir + "/" + name + ".img", kvp = dir + "/" + name + ".kv";
        FILE* f = std::fopen(img.c_str(), "wb"); if (!f) { std::printf("cannot write %s\n", img.c_str()); return 1; }
        std::fwrite(dev.raw(0), PAGE, s.used_pages(), f); std::fclose(f);
        FILE* kv = std::fopen(kvp.c_str(), "w");
        std::fprintf(kv, "# %s pages=%u\n", name, s.used_pages());
        dump(s, kv, 0);
        if (snapv) { std::fprintf(kv, "@snap %u\n", (unsigned)snapv); dump(s, kv, snapv); }
        std::fclose(kv);
        std::printf("%s: %u pages, runs=%u\n", name, s.used_pages(), s.run_count());
    }
    return 0;
}
