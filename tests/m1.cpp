// m1：run 元数据页（PK_RMETA）的立身之本 —— 三条断言。
//   1) 正常重开：meta 命中数 > 0，数据/校验全对（不扫全库也能挂对）；
//   2) meta 页被篡改（翻转一个字节）：CRC 挡下来 ⇒ 退回逐页扫描，仍然挂载正确；
//   3) 老格式（把所有 meta 页清零）：同样退回扫描，一条记录都不许错。
// 之所以要它：元数据页是「派生量的持久化副本」，一旦哪天真丢了，读路径必须
// 依然能靠数据页自证（fences/pbytes/sp/bloom 都能重建）。这个测试守的就是这个退路。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace fcs;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL %s\n", msg); ++fails; } } while (0)

static const int N = 60000;          // 60000 条 / 每 3000 提交 = 20 个 run，每个 ~19 页 ⇒ 都会写 meta
static const int CE = 3000;
static std::vector<u32> keys;
static std::vector<u32> lens;

static std::vector<u32> find_meta_pages(RamDev& dev, u32 used) {
    std::vector<u32> v;
    for (u32 p = 2; p < used; ++p)
        if (rd32(dev.raw(p)) == PK_RMETA) v.push_back(p);
    return v;
}

// 全量回读：键/长度/内容逐条比对
static void check_all(Store& s, const char* tag) {
    int bad = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        u8 buf[64]; u16 len = 0;
        RC rc = s.get(keys[i], buf, sizeof buf, &len);
        if (rc != RC::OK || len != lens[i]) { if (bad++ < 3) std::printf("    %s k=%u rc=%s len=%u want=%u\n", tag, keys[i], rc_str(rc), len, lens[i]); continue; }
        u8 want = (u8)(keys[i] * 31 + 7);
        if (buf[0] != want || buf[len - 1] != want) { if (bad++ < 3) std::printf("    %s k=%u content\n", tag, keys[i]); }
    }
    CHECK(bad == 0, tag);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    static RamDev dev(4096);
    u8 arena[256 * PAGE];
    keys.reserve(N); lens.reserve(N);
    u32 used = 0;

    {   // ---- 写入 ----
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
        u8 v[64];
        for (int i = 0; i < N; ++i) {
            u32 k = (u32)(i * 7 + 1);
            u16 len = (u16)(8 + (i % 24));
            u8 fill = (u8)(k * 31 + 7);
            std::memset(v, fill, sizeof v);
            CHECK(s.put(k, v, len) == RC::OK, "put");
            keys.push_back(k); lens.push_back(len);
            if (i % CE == CE - 1) s.commit();
        }
        s.commit();
        used = s.used_pages();
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "write verify");
        std::printf("  写入 %d 条：used=%u 页 runs=%u cells=%u\n", N, used, r.runs, r.cells);
    }

    std::vector<u32> metas = find_meta_pages(dev, used);
    CHECK(!metas.empty(), "应当写了元数据页");
    if (metas.empty()) { std::printf("FAILURES: %d\n", fails); return 1; }
    std::printf("  元数据页：%zu 个\n", metas.size());

    {   // ---- 1) 正常重开 ----
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "reopen");
        CHECK(s.meta_hits() > 0, "重开应当命中 meta");
        std::printf("  重开：meta_hits=%llu runs=%u\n", (unsigned long long)s.meta_hits(), s.run_count());
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "reopen verify");
        check_all(s, "重开后回读");
    }

    {   // ---- 2) 篡改一个 meta 页：CRC 必须挡住 ----
        u32 pid = metas[metas.size() / 2];
        u8* raw = dev.raw(pid);
        raw[PAGE / 2] ^= 0x5A;                   // 翻载荷里的一位
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "tampered reopen");
        std::printf("  篡改 meta 页 %u 后：meta_hits=%llu runs=%u\n", pid,
                    (unsigned long long)s.meta_hits(), s.run_count());
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "tampered verify");
        check_all(s, "篡改后回读");
    }

    {   // ---- 3) 全清成老格式：所有 meta 页清零 ----
        for (u32 p : metas) {
            u8* raw = dev.raw(p);
            std::memset(raw + PAGE / 4, 0, PAGE / 4);   // 只清一半，坏得更刁钻
        }
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "old-format reopen");
        std::printf("  清掉 meta 后：meta_hits=%llu runs=%u\n",
                    (unsigned long long)s.meta_hits(), s.run_count());
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "old-format verify");
        check_all(s, "老格式回读");
        // 还能继续写（旧 run 的扫描结果与正常路径无差别）
        u8 v[16]; std::memset(v, 9, sizeof v);
        for (int i = 0; i < 3000; ++i) s.put((u32)(N * 7 + i + 1), v, 16);
        CHECK(s.commit() == RC::OK, "旧格式下继续提交");
        Report r2; s.verify(&r2);
        CHECK(r2.errors == 0, "旧格式下 verify");
    }

    {   // ---- 4) 自愈：清掉 meta → 一次 checkpoint 之后重开应当重新命中 ----
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "heal open");
        CHECK(s.meta_hits() == 0, "清掉之后不该有命中");
        CHECK(s.checkpoint() == RC::OK, "checkpoint（自愈）");
        std::printf("  checkpoint 之后：runs=%u\n", s.run_count());
    }
    {
        Store s;
        CHECK(s.open(&dev, arena, 256) == RC::OK, "heal reopen");
        std::printf("  自愈后重开：meta_hits=%llu runs=%u\n",
                    (unsigned long long)s.meta_hits(), s.run_count());
        CHECK(s.meta_hits() > 0, "checkpoint 应当把 meta 补回来");
        Report r; s.verify(&r);
        CHECK(r.errors == 0, "heal verify");
        check_all(s, "自愈后回读");
    }

    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
