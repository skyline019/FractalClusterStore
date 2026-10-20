// h1 —— 挂载模糊（第 11 轮 H1，P2 格式升版的前置护栏）
//
// 对一个合法的库镜像随机翻字节（超级块 / 清单 / meta / 数据 / 溢出页都可能中），然后 open：
//   结果只允许 OK（回落到旧提交点或本提交点自愈）或 CORRUPT / IO（明确拒绝）；
//   open 成功之后 get / select / verify / 再 commit 一次都不许崩、不许挂（整段有 alarm 兜底）。
// 不断言"读到的值一定是最新"——翻坏的可能正是最新清单，回落旧提交是允许的；断言的是
//   (a) 过程码在白名单里；
//   (b) **没有查不出来的坏数据**：数据页 CRC 按设计只在 verify/scrub 查、get 不查（读路径不付 CRC 代价），
//       所以 get 读到假版本（值 = [tag(4B)] 重复、tag ∉ 该键写过的集合）只在 verify 也报错时允许；
//       verify 干净却读到假版本 ⇒ CRC 覆盖有洞 ⇒ FAIL。溢出大值读路径自带 CRC：只能是真实版本或 CORRUPT；
//   (c) 没有 ASan/UBSan 报警（make asan 里同样跑）。
// 用法：build/h1 [iters=400] [seed=1]；默认 400 次 ≈ 3 s（RamDev，2 核沙箱）。
#include "fcs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <unistd.h>
#include <vector>
using namespace fcs;

namespace {
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Image { std::vector<u8> bytes; u32 pages = 0; std::map<u64, std::set<u32>> tags; };

// 造一个"什么都有"的小库：小值、2 条溢出大值、点删、范围删、快照、一次归一、几次提交
Image build_image() {
    Image im;
    static u8 arena[256 * PAGE];
    RamDev dev(2048);
    Store s; if (s.open(&dev, arena, 256) != RC::OK) return im;
    std::mt19937_64 rng(42);
    auto put = [&](u64 k, u32 tag, u32 len) {
        std::vector<u8> v(len); for (u32 i = 0; i < len; i += 4) std::memcpy(v.data() + i, &tag, std::min<u32>(4, len - i));
        s.put(k, v.data(), (u16)len); im.tags[k].insert(tag);
    };
    for (int c = 0; c < 8; ++c) {
        for (int i = 0; i < 120; ++i) put(1 + rng() % 1500, 100 + c, 20 + rng() % 180);
        if (c == 2) { put(5000, 777, 9000); put(5001, 778, 20000); }           // 溢出链
        if (c == 4) { for (u64 k = 300; k < 330; ++k) { s.del(k); im.tags[k].insert(0); } }
        if (c == 5) { s.del_range(600, 650); for (u64 k = 600; k < 650; ++k) im.tags[k].insert(0); }
        s.commit();
        if (c == 3) s.snapshot();
        if (c == 6) s.consolidate();
    }
    im.pages = s.used_pages();
    im.bytes.assign(dev.raw(0), dev.raw(0) + (size_t)im.pages * PAGE);
    return im;
}

bool value_ok(const Image& im, u64 k, const u8* v, u16 len) {
    auto it = im.tags.find(k);
    if (it == im.tags.end()) return false;             // 从没写过的键读出了值
    if (len < 4) return false;
    u32 tag; std::memcpy(&tag, v, 4);
    if (!it->second.count(tag)) return false;
    for (u32 i = 4; i + 4 <= len; i += 4) if (std::memcmp(v + i, &tag, 4) != 0) return false;   // 半页/串页
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? atoi(argv[1]) : 400;
    const int seed = argc > 2 ? atoi(argv[2]) : 1;
    alarm(240);                                        // 挂住 = 失败（SIGALRM 杀进程），不拖死 make test
    double t0 = now_s();
    Image im = build_image();
    if (im.pages == 0) { std::printf("h1 FAIL: 造镜像失败\n"); return 1; }
    std::mt19937_64 rng(seed);
    int n_ok = 0, n_corrupt = 0, n_io = 0, n_other = 0, bad = 0, n_verify_bad = 0, n_val_bad = 0;
    static u8 arena[256 * PAGE];
    std::vector<u8> out(70000);
    for (int it = 0; it < iters; ++it) {
        RamDev dev(2048);
        std::memcpy(dev.raw(0), im.bytes.data(), im.bytes.size());
        // 翻字节：1/3 打超级块（页 0/1），1/3 打低页（清单/小 run 多在这里），1/3 全镜像随机；每次 1 ~ 16 个字节
        const int nflip = 1 + (int)(rng() % 16);
        for (int f = 0; f < nflip; ++f) {
            u32 pg; const u64 m = rng() % 3;
            if (m == 0) pg = (u32)(rng() % 2);
            else if (m == 1) pg = (u32)(rng() % std::min<u32>(im.pages, 16));
            else pg = (u32)(rng() % im.pages);
            u8* p = dev.raw(pg) + rng() % PAGE;
            *p ^= (u8)(1 + rng() % 255);
        }
        Store s;
        RC rc = s.open(&dev, arena, 256);
        if (rc == RC::OK) ++n_ok;
        else if (rc == RC::CORRUPT) ++n_corrupt;
        else if (rc == RC::IO) ++n_io;
        else { ++n_other; if (bad++ < 5) std::printf("  h1 FAIL: iter %d open rc=%s 不在白名单 {OK, CORRUPT, IO}\n", it, rc_str(rc)); }
        if (rc != RC::OK) continue;
        // 挂上了：读 + 扫 + 校验 + 再写一次，只要不崩不挂、坏数据查得出来即可
        Report rep; RC v = s.verify(&rep);
        const bool detected = v != RC::OK || rep.errors;
        if (detected) ++n_verify_bad;
        u16 len = 0;
        for (int i = 0; i < 60; ++i) {
            u64 k = i < 2 ? 5000 + i : 1 + rng() % 1500;
            RC g = s.get(k, out.data(), 65535, &len);
            if (g == RC::OK && !value_ok(im, k, out.data(), len)) {
                if (k >= 5000 || !detected) { if (bad++ < 5) std::printf("  h1 FAIL: iter %d get(%llu) 读到假版本 (len=%u) 而 verify %s\n", it, (unsigned long long)k, len, detected ? "已报错（溢出值读路径应自带 CRC）" : "没报错（查不出来的坏数据）"); }
                else ++n_val_bad;
            } else if (g != RC::OK && g != RC::NOTFOUND && g != RC::CORRUPT && g != RC::IO) { if (bad++ < 5) std::printf("  h1 FAIL: iter %d get rc=%s\n", it, rc_str(g)); }
        }
        u32 cnt = 0;
        s.select(1, 2000, [&](u64, const void*, u16) { ++cnt; return true; });
        u32 tag = 999; s.put(1, &tag, 4); s.commit();     // 再写一次也不许崩
    }
    std::printf("  h1 挂载模糊 %d 次（seed %d，镜像 %u 页）：OK %d / CORRUPT %d / IO %d / 其它 %d；成功挂载后 verify 报错 %d 次、同时 get 读到假版本 %d 次（都查得出来）\n",
                iters, seed, im.pages, n_ok, n_corrupt, n_io, n_other, n_verify_bad, n_val_bad);
    std::printf("h1 挂载模糊: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
