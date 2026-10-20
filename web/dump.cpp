// dump.cpp —— 把一次真实提交后的盘上字节 dump 成 JSON，喂给前端动画页做“真字节”展示。
// 输出：超级块两槽的字段、清单页头 + 前 3 条条目、最新数据页头 + 前 4 条记录（含十六进制）。
#include "fcs.h"
#include <cstdio>
using namespace fcs;
static u8 arena[512 * PAGE];

static void hex(const u8* p, int n) {
    for (int i = 0; i < n; ++i) std::printf("%02x", p[i]);
}

int main() {
    static RamDev dev(512);
    static Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { std::printf("open fail\n"); return 1; }
    for (u64 k = 1000; k <= 1009; ++k) {
        u8 v[24];
        std::memset(v, (int)(k & 0xff), sizeof v);
        s.put(k, v, (u16)(8 + (k % 13)));
    }
    s.commit();
    for (u64 k = 1000; k <= 1002; ++k) { u8 v[16]; std::memset(v, 'O', 16); s.put(k, v, 12); }
    s.del(1009);
    s.commit();
    // 再提交 40 次：撑爆超级块内联增量（32 条），逼出真正的清单页
    for (int r = 0; r < 40; ++r) {
        for (u64 k = 2000; k <= 2002; ++k) { u8 v[8]; std::memset(v, (u8)r, 8); s.put(k + (u64)r * 10, v, 8); }
        s.commit();
    }
    u32 used = s.used_pages(), run = 0;
    u32 man = rd32(dev.raw(0) + 24);      // SB 槽 A 的 man_last = 清单链头（最新一页）
    for (u32 p = 2; p < used; ++p)
        if (rd32(dev.raw(p)) == PK_DATA && p > run) run = p;
    Report rep;
    s.verify(&rep);

    std::printf("{\n\"used_pages\":%u,\"report\":{\"cells\":%u,\"runs\":%u,\"recs\":%u,\"live\":%llu,\"errors\":%u},\n",
                used, rep.cells, rep.runs, rep.recs, (unsigned long long)rep.live, rep.errors);
    std::printf("\"sb\":[\n");
    for (u32 slot = 0; slot < 2; ++slot) {
        const u8* b = dev.raw(slot);
        std::printf("%s\n {\"slot\":%u,\"hex\":\"", slot ? "," : "", slot); hex(b, 56); std::printf("\",");
        std::printf("\"magic\":%u,\"vers\":%u,\"state\":%u,\"chk\":%u,\"tail\":%u,\"man_first\":%u,"
                    "\"man_last\":%u,\"nman\":%u,\"n_inl\":%u,%s}",
                    rd32(b + 0), rd32(b + 4), rd32(b + 8), rd32(b + 12), rd32(b + 16), rd32(b + 20),
                    rd32(b + 24), rd32(b + 28), rd32(b + 32),
                    slot == 0 ? "\"note\":\"最近一次提交\"" : "\"note\":\"上一次提交\"");
    }
    std::printf(" ],\n");
    {
        const u8* b = dev.raw(man);
        std::printf("\"man\":{\"pid\":%u,\"prev\":%u,\"n\":%u,\"crc\":%u,\"hex\":\"", man, rd32(b + 4), rd32(b + 8), rd32(b + 12));
        hex(b, 176);
        std::printf("\",\"ents\":[");
        u32 n = rd32(b + 8);
        if (n > 32) n = 32;
        for (u32 i = 0; i < n && i < 3; ++i) {
            const u8* e = b + MHDR + i * MENT;
            std::printf("%s{\"op\":%u,\"lvl\":%u,\"ver\":%u,\"key\":%llu,\"last\":%llu,\"page0\":%u,"
                        "\"npages\":%u,\"nbytes\":%u,\"seq\":%u}",
                        i ? "," : "", e[0], e[1], rd32(e + 4),
                        (unsigned long long)rd64(e + 8), (unsigned long long)rd64(e + 16),
                        rd32(e + 24), rd32(e + 28), rd32(e + 32), rd32(e + 36));
        }
        std::printf("]},\n");
    }
    {
        const u8* b = dev.raw(run);
        u16 nrec = rd16(b + 32), usd = rd16(b + 34);
        std::printf("\"data\":{\"pid\":%u,\"next\":%u,\"chk\":%u,\"ver\":%u,\"first\":%llu,\"last\":%llu,"
                    "\"nrec\":%u,\"used\":%u,\"pfx\":%u,\"fmt\":%u,\"hex\":\"",
                    run, rd32(b + 4), rd32(b + 8), rd32(b + 12), (unsigned long long)rd64(b + 16),
                    (unsigned long long)rd64(b + 24), nrec, usd, b[36], b[37]);
        hex(b, 96);
        std::printf("\",\"recs\":[");
        u32 off = DHDR;
        u8 pfx = pg_pfx(b);
        for (u32 i = 0; i < 4 && off < usd; ++i) {
            RecHdr h;
            rd_rec(b, off, h);
            std::printf("%s{\"off\":%u,\"key\":%llu,\"len\":%u,\"size\":%u}",
                        i ? "," : "", off, (unsigned long long)h.key, h.len, pg_rec_size(pfx, h.len));
            off += pg_rec_size(pfx, h.len);
        }
        std::printf("]}\n}\n");
    }
    return 0;
}
