// h2 —— golden 镜像兼容性（第 11 轮 H2；第 16 轮 P2 扩成两套）：
//   tests/golden/*.img     = 当前格式（v2，第 16 轮 gen.cpp 重新生成）：挂载 → verify → 逐键比对（当前视图 + 快照视图）→ 反向 NOTFOUND 抽查。
//   tests/golden/v1/*.img  = 第 11 轮固化的 v1 镜像（老库）：
//     (a) 只读阶段：挂载即可读（mounted_format()==1、format_version()==1，一个字节都不写回）、verify、逐键比对；
//     (b) 升级阶段：写一个新键 + commit ⇒ format_version()==2（第一次写 SB 就是 v2，清单链里老页照读）；再从同一设备重开 ⇒
//         mounted_format()==2，verify 干净、老键全对、新键在。
// 格式一旦再变这里会先红：要么新代码仍能读老格式（兼容），要么有意升版并重新生成镜像（gen.cpp）并在文档里说明。
// 镜像装进 RamDev 后挂载，不写回文件；用法：build/h2 [dir=tests/golden]
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace fcs;

static u64 fnv(const u8* p, size_t n) { u64 h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }

// 逐键比对 .kv；with_snap=false 时跳过 @snap 之后的快照视图（升级后快照早已释放、老 run 可能被合并掉）
static int compare_kv(Store& s, const std::string& kvp, const char* name, const char* phase, bool with_snap, u32* nkeys_out = nullptr) {
    FILE* kv = std::fopen(kvp.c_str(), "r");
    if (!kv) { std::printf("  h2 FAIL: 打不开 %s\n", kvp.c_str()); return 1; }
    char line[256]; u64 view = 0; u32 nkeys = 0, nsnap = 0; int bad = 0;
    static u8 out[70000]; u16 len = 0;
    std::vector<char> seen(6001, 0);
    while (std::fgets(line, sizeof line, kv)) {
        if (line[0] == '#') continue;
        if (line[0] == '@') { if (!with_snap) break; unsigned long long v = 0; std::sscanf(line, "@snap %llu", &v); view = v; std::fill(seen.begin(), seen.end(), 0); continue; }
        unsigned long long k = 0, h = 0; unsigned l = 0;
        if (std::sscanf(line, "%llu %u %llu", &k, &l, &h) != 3) continue;
        if (k <= 6000) seen[k] = 1;
        RC g = view ? s.get_at(k, view, out, 65535, &len) : s.get(k, out, 65535, &len);
        if (g != RC::OK || len != l || fnv(out, len) != h) { if (bad++ < 3) std::printf("  h2 FAIL: %s/%s key %llu view %llu rc=%s len=%u want %u\n", name, phase, k, (unsigned long long)view, rc_str(g), len, l); }
        view ? ++nsnap : ++nkeys;
    }
    std::fclose(kv);
    // 反向：.kv 里没有的键必须 NOTFOUND（最后一个视图抽查全部 1..6000；升级阶段写的新键用 6500，不在此范围）
    for (u64 k = 1; k <= 6000; ++k) if (!seen[k]) {
        RC g = view ? s.get_at(k, view, out, 65535, &len) : s.get(k, out, 65535, &len);
        if (g != RC::NOTFOUND) { if (bad++ < 3) std::printf("  h2 FAIL: %s/%s key %llu 应为 NOTFOUND，rc=%s\n", name, phase, (unsigned long long)k, rc_str(g)); }
    }
    if (nkeys_out) *nkeys_out = nkeys;
    return bad;
}

static bool load_img(const std::string& img, RamDev& dev, u32& pages) {
    FILE* f = std::fopen(img.c_str(), "rb");
    if (!f) return false;
    std::vector<u8> bytes; u8 buf[PAGE]; size_t n;
    while ((n = std::fread(buf, 1, PAGE, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    pages = (u32)(bytes.size() / PAGE);
    std::memcpy(dev.raw(0), bytes.data(), bytes.size());
    return true;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/golden";
    int bad = 0;
    // ---- v2（当前格式）
    for (const char* name : {"small", "bigval", "tomb_snap"}) {
        std::string img = dir + "/" + name + ".img", kvp = dir + "/" + name + ".kv";
        RamDev dev(4096); u32 pages = 0;
        if (!load_img(img, dev, pages)) { std::printf("  h2 FAIL: 打不开 %s（请在仓库根目录运行）\n", img.c_str()); bad++; continue; }
        static u8 arena[256 * PAGE];
        Store s; RC rc = s.open(&dev, arena, 256);
        if (rc != RC::OK) { std::printf("  h2 FAIL: %s open rc=%s\n", name, rc_str(rc)); bad++; continue; }
        int b0 = bad;
        if (s.mounted_format() != 2 || s.format_version() != 2) { std::printf("  h2 FAIL: %s 应为 v2（mounted=%u fmt=%u）\n", name, s.mounted_format(), s.format_version()); bad++; }
        Report rep; rc = s.verify(&rep);
        if (rc != RC::OK || rep.errors) { std::printf("  h2 FAIL: %s verify rc=%s errors=%u first=%s\n", name, rc_str(rc), rep.errors, rep.first_err); bad++; }
        u32 nkeys = 0;
        bad += compare_kv(s, kvp, name, "v2", true, &nkeys);
        std::printf("  h2 v2 %-9s %3u 页 runs=%u：当前视图 %u 键 verify=%s  %s\n", name, pages, s.run_count(), nkeys, rep.errors ? "FAIL" : "OK", bad > b0 ? "FAIL" : "ok");
    }
    // ---- v1（老库）：只读 → 升级 → 重开
    for (const char* name : {"small", "bigval", "tomb_snap"}) {
        std::string img = dir + "/v1/" + name + ".img", kvp = dir + "/v1/" + name + ".kv";
        RamDev dev(4096); u32 pages = 0;
        if (!load_img(img, dev, pages)) { std::printf("  h2 FAIL: 打不开 %s\n", img.c_str()); bad++; continue; }
        std::vector<u8> before(dev.raw(0), dev.raw(0) + (size_t)pages * PAGE);
        static u8 arena[256 * PAGE];
        int b0 = bad; u32 nkeys = 0; Report rep;
        {
            Store s; RC rc = s.open(&dev, arena, 256);
            if (rc != RC::OK) { std::printf("  h2 FAIL: v1 %s open rc=%s\n", name, rc_str(rc)); bad++; continue; }
            if (s.mounted_format() != 1 || s.format_version() != 1) { std::printf("  h2 FAIL: v1 %s 应识别为 v1（mounted=%u fmt=%u）\n", name, s.mounted_format(), s.format_version()); bad++; }
            rc = s.verify(&rep);
            if (rc != RC::OK || rep.errors) { std::printf("  h2 FAIL: v1 %s verify rc=%s errors=%u first=%s\n", name, rc_str(rc), rep.errors, rep.first_err); bad++; }
            bad += compare_kv(s, kvp, name, "v1-ro", true, &nkeys);
            // 只读阶段一个字节都不写回
            if (std::memcmp(before.data(), dev.raw(0), before.size()) != 0) { std::printf("  h2 FAIL: v1 %s 只读挂载改了镜像\n", name); bad++; }
            if (s.format_version() != 1) { std::printf("  h2 FAIL: v1 %s 只读阶段不该升级（fmt=%u）\n", name, s.format_version()); bad++; }
            // 升级：第一次写 SB 就是 v2
            const u32 tag = 0xA5A5A5A5u;
            if (s.put(6500, &tag, 4) != RC::OK || s.commit() != RC::OK) { std::printf("  h2 FAIL: v1 %s 升级写入失败\n", name); bad++; }
            if (s.format_version() != 2) { std::printf("  h2 FAIL: v1 %s 提交后仍是 v%u\n", name, s.format_version()); bad++; }
            rc = s.verify(&rep);
            if (rc != RC::OK || rep.errors) { std::printf("  h2 FAIL: v1 %s 升级后 verify rc=%s errors=%u first=%s\n", name, rc_str(rc), rep.errors, rep.first_err); bad++; }
            bad += compare_kv(s, kvp, name, "v1-upgraded", false);
        }
        {   // 重开：SB 已是 v2，清单链里可能新老页混着
            static u8 arena2[256 * PAGE];
            Store s; RC rc = s.open(&dev, arena2, 256);
            if (rc != RC::OK) { std::printf("  h2 FAIL: v1 %s 升级后重开 rc=%s\n", name, rc_str(rc)); bad++; continue; }
            if (s.mounted_format() != 2) { std::printf("  h2 FAIL: v1 %s 重开应为 v2（mounted=%u）\n", name, s.mounted_format()); bad++; }
            rc = s.verify(&rep);
            if (rc != RC::OK || rep.errors) { std::printf("  h2 FAIL: v1 %s 重开 verify rc=%s errors=%u first=%s\n", name, rc_str(rc), rep.errors, rep.first_err); bad++; }
            bad += compare_kv(s, kvp, name, "v1-reopen", false);
            u32 got = 0; u16 len = 0;
            if (s.get(6500, &got, 4, &len) != RC::OK || len != 4 || got != 0xA5A5A5A5u) { std::printf("  h2 FAIL: v1 %s 重开后新键丢了\n", name); bad++; }
            std::printf("  h2 v1 %-9s %3u 页：只读 %u 键 → 升级（v1→v2）→ 重开 runs=%u verify=%s  %s\n", name, pages, nkeys, s.run_count(), rep.errors ? "FAIL" : "OK", bad > b0 ? "FAIL" : "ok");
        }
    }
    std::printf("h2 golden 镜像兼容（v2 三件套 + v1 老库只读/升级/重开）: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
