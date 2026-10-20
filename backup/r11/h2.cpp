// h2 —— golden 镜像兼容性（第 11 轮 H2）：tests/golden/*.img 是用**当时的格式**固化的小库，
// 每次 make test 挂载它们、逐键比对 tests/golden/*.kv（当前视图 + 快照视图）、verify 必须干净。
// 格式一旦变了（P2 轮）这里会先红：要么新代码仍能读老格式（兼容），要么有意升版并重新生成镜像（gen.cpp）并在文档里说明。
// 镜像只读挂载后不写回文件；用法：build/h2 [dir=tests/golden]
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace fcs;

static u64 fnv(const u8* p, size_t n) { u64 h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/golden";
    int bad = 0;
    for (const char* name : {"small", "bigval", "tomb_snap"}) {
        std::string img = dir + "/" + name + ".img", kvp = dir + "/" + name + ".kv";
        FILE* f = std::fopen(img.c_str(), "rb");
        if (!f) { std::printf("  h2 FAIL: 打不开 %s（请在仓库根目录运行）\n", img.c_str()); bad++; continue; }
        std::vector<u8> bytes; u8 buf[PAGE]; size_t n;
        while ((n = std::fread(buf, 1, PAGE, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
        const u32 pages = (u32)(bytes.size() / PAGE);
        RamDev dev(4096);
        std::memcpy(dev.raw(0), bytes.data(), bytes.size());
        static u8 arena[256 * PAGE];
        Store s; RC rc = s.open(&dev, arena, 256);
        if (rc != RC::OK) { std::printf("  h2 FAIL: %s open rc=%s\n", name, rc_str(rc)); bad++; continue; }
        Report rep; rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  h2 FAIL: %s verify rc=%s errors=%u first=%s\n", name, rc_str(rc), rep.errors, rep.first_err); bad++; }
        FILE* kv = std::fopen(kvp.c_str(), "r");
        if (!kv) { std::printf("  h2 FAIL: 打不开 %s\n", kvp.c_str()); bad++; continue; }
        char line[256]; Vers view = 0; u32 nkeys = 0, nsnap = 0; int b0 = bad;
        static u8 out[70000]; u16 len = 0;
        std::vector<char> seen(6001, 0);
        while (std::fgets(line, sizeof line, kv)) {
            if (line[0] == '#') continue;
            if (line[0] == '@') { unsigned v = 0; std::sscanf(line, "@snap %u", &v); view = (Vers)v; std::fill(seen.begin(), seen.end(), 0); continue; }
            unsigned long long k = 0, h = 0; unsigned l = 0;
            if (std::sscanf(line, "%llu %u %llu", &k, &l, &h) != 3) continue;
            if (k <= 6000) seen[k] = 1;
            RC g = view ? s.get_at(k, view, out, 65535, &len) : s.get(k, out, 65535, &len);
            if (g != RC::OK || len != l || fnv(out, len) != h) { if (bad++ - b0 < 3) std::printf("  h2 FAIL: %s key %llu view %u rc=%s len=%u want %u\n", name, k, (unsigned)view, rc_str(g), len, l); }
            view ? ++nsnap : ++nkeys;
        }
        std::fclose(kv);
        // 反向：.kv 里没有的键必须 NOTFOUND（当前视图抽查全部 1..6000；快照视图同样）
        for (u64 k = 1; k <= 6000; ++k) if (!seen[k]) {
            RC g = view ? s.get_at(k, view, out, 65535, &len) : s.get(k, out, 65535, &len);
            if (g != RC::NOTFOUND) { if (bad++ - b0 < 3) std::printf("  h2 FAIL: %s key %llu 应为 NOTFOUND，rc=%s\n", name, (unsigned long long)k, rc_str(g)); }
        }
        std::printf("  h2 %-9s %3u 页 runs=%u：当前视图 %u 键%s verify=%s  %s\n", name, pages, s.run_count(), nkeys,
                    nsnap ? (std::string(" + 快照视图 ") + std::to_string(nsnap) + " 键").c_str() : "", rep.errors ? "FAIL" : "OK", bad > b0 ? "FAIL" : "ok");
    }
    std::printf("h2 golden 镜像兼容: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
