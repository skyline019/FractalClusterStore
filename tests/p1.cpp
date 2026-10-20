// p1：页内前缀压缩 —— 有序键必须真省字节，随机键必须与旧格式一致（pfx=0）
#include "fcs.h"
#include <cstdio>
#include <vector>
using namespace fcs;
static u8 arena[4096 * PAGE];

struct Stat { u32 pages, compressed, used, recs; u32 minpfx, maxpfx; };

static Stat scan_pages(RamDev& dev) {
    Stat st{}; st.minpfx = 99;
    for (PageId p = 0; p < dev.capacity(); ++p) {
        const u8* pg = dev.raw(p);                     // 只读直取，测试用
        if (!pg || rd32(pg) != PK_DATA) continue;
        u32 n = rd16(pg + 32);
        if (!n) continue;
        u8 f = pg_pfx(pg);
        ++st.pages;
        if (f) ++st.compressed;
        if (f < st.minpfx) st.minpfx = f;
        if (f > st.maxpfx) st.maxpfx = f;
        st.used += (u32)rd16(pg + 34) - DHDR;
        st.recs += n;
    }
    return st;
}

static int one(bool ordered, const char* tag) {
    RamDev dev(8192);
    Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { std::printf("open\n"); return 1; }
    const u32 N = 40000;
    std::vector<u8> v(32, 0x5a);
    for (u32 i = 0; i < N; ++i) {
        u64 k = ordered ? (1700000000000ull + (u64)i * 7) : (mix64(i * 0x9e3779b97f4a7c15ull) & 0x7FFFFFFFFFFFFFFFull);
        if (s.put(k, v.data(), (u32)v.size()) != RC::OK) { std::printf("put %u\n", i); return 1; }
    }
    if (s.commit() != RC::OK) { std::printf("commit\n"); return 1; }
    Stat st = scan_pages(dev);
    double bpr = st.recs ? (double)st.used / st.recs : 0;
    std::printf("%s: 页=%u 压缩页=%u pfx=[%u,%u] 条=%u %.1f B/条\n",
                tag, st.pages, st.compressed, st.minpfx == 99 ? 0 : st.minpfx, st.maxpfx, st.recs, bpr);

    Report rep;
    RC rc = s.verify(&rep);
    if (rc != RC::OK || rep.errors) { std::printf("%s: verify rc=%d bad=%u (%s)\n", tag, (int)rc, rep.errors, rep.first_err); return 1; }
    for (u32 i = 0; i < N; i += 997) {
        u64 k = ordered ? (1700000000000ull + (u64)i * 7) : (mix64(i * 0x9e3779b97f4a7c15ull) & 0x7FFFFFFFFFFFFFFFull);
        u8 out[64]; u16 l = 0;
        if (s.get(k, out, sizeof out, &l) != RC::OK || l != v.size()) { std::printf("%s: get %u 失败\n", tag, i); return 1; }
    }
    // 覆盖写一遍，确认压缩页上的更新仍然正确
    for (u32 i = 0; i < N; i += 3) {
        u64 k = ordered ? (1700000000000ull + (u64)i * 7) : (mix64(i * 0x9e3779b97f4a7c15ull) & 0x7FFFFFFFFFFFFFFFull);
        s.put(k, v.data(), 17);                        // 换成长度不同的值
    }
    if (s.commit() != RC::OK) { std::printf("commit2\n"); return 1; }
    if (s.verify(&rep) != RC::OK || rep.errors) { std::printf("%s: verify2 bad=%u (%s)\n", tag, rep.errors, rep.first_err); return 1; }
    std::printf("%s: errors=%u 记录=%u\n", tag, rep.errors, rep.recs);
    return 0;
}

// 老格式兼容：fmt=0（页头 36/37 为 0）⇒ pfx=0 ⇒ 与旧记录布局逐字节相同
static int old_format_compat() {
    std::vector<u8> pg(PAGE, 0);
    wr32(pg.data(), PK_DATA);
    const u64 keys[3] = {7, 9, 11};
    u32 off = DHDR;
    for (u64 k : keys) {
        std::memcpy(pg.data() + off, &k, 8);          // 旧布局：完整 8 B 键
        u16 l = 5;
        std::memcpy(pg.data() + off + 8, &l, 2);
        off += rec_size(l);                            // 旧 rec_size
    }
    std::memcpy(pg.data() + 16, &keys[0], 8);
    std::memcpy(pg.data() + 24, &keys[2], 8);
    wr16(pg.data() + 32, 3); wr16(pg.data() + 34, (u16)off);
    // 36/37 保持 0 —— 就是老库的样子
    if (pg_fmt(pg.data()) != 0 || pg_pfx(pg.data()) != 0) { std::printf("老格式识别失败\n"); return 1; }
    u32 o = DHDR;
    for (u64 k : keys) {
        RecHdr h;
        rd_rec(pg.data(), o, h);
        if (h.key != k) { std::printf("老格式键错: %llu != %llu\n", (unsigned long long)h.key, (unsigned long long)k); return 1; }
        if (pg_rec_size(0, h.len) != rec_size(h.len)) { std::printf("老格式尺寸不一致\n"); return 1; }
        o += pg_rec_size(0, h.len);
    }
    if (o != off) { std::printf("老格式长度错\n"); return 1; }
    std::printf("老格式兼容: ok（fmt=0/pfx=0，尺寸与布局一致）\n");
    return 0;
}

int main() {
    if (old_format_compat()) return 1;
    if (one(true, "有序键")) return 1;
    if (one(false, "随机键")) return 1;
    std::printf("PASS\n");
    return 0;
}
