// f3：掉电/崩溃注入（NOR 语义设备）+ 损坏检测（CRC）
//   1) NorSimDev 上跑通写-读-重开（只擦 SB 槽，数据页只写一次）
//   2) 在提交的每一次写操作处注入失败：重开后必须仍是“上一个已提交状态”
//   3) 数据页/清单页翻转一字节：verify 报 CORRUPT，读不返回垃圾
#include "fcs.h"
#include <cstdio>
#include <map>
#include <vector>
using namespace fcs;
static u8 arena[512 * PAGE];

static bool get_i(Store& s, u64 k, u32& x) {
    u8 b[32]; u16 l = 0;
    if (s.get(k, b, sizeof b, &l) != RC::OK || l != 12) return false;
    std::memcpy(&x, b, 4);
    return true;
}
static void put_i(Store& s, u64 k, u32 x, int tag) {
    u8 b[12];
    std::memcpy(b, &x, 4);
    std::memcpy(b + 4, &tag, 4);
    std::memset(b + 8, 0x5A, 4);
    s.put(k, b, 12);
}

// 可注入故障的 RAM 设备：第 N 次写失败
struct FaultDev : BlockDev {
    std::vector<u8> buf;
    PageId np;
    int fail_at = -1, wcount = 0;
    explicit FaultDev(PageId n) : buf((size_t)n * PAGE, 0), np(n) {}
    RC do_read(PageId pid, u32 off, void* dst, u32 len) override {
        if (pid >= np || off + len > PAGE) return RC::IO;
        st.read_calls++; st.read_bytes += len;
        std::memcpy(dst, &buf[(size_t)pid * PAGE + off], len);
        return RC::OK;
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (pid >= np || off + len > PAGE) return RC::IO;
        if (++wcount == fail_at) return RC::IO;
        st.writes++; st.write_bytes += len;
        std::memcpy(&buf[(size_t)pid * PAGE + off], src, len);
        return RC::OK;
    }
    PageId capacity() const override { return np; }
};

int main() {
    int fails = 0;

    // ---------------------------------------------------------------- 1) NOR 设备
    {
        NorSimDev dev(2048);
        Store s;
        if (s.open(&dev, arena, 256) != RC::OK) { std::printf("nor open\n"); return 1; }
        for (int r = 0; r < 4; ++r) {
            for (int i = 0; i < 200; ++i) put_i(s, (u64)(r * 1000 + i), (u32)(r * 100 + i), r);
            if (s.commit() != RC::OK) { std::printf("nor commit %d\n", r); return 1; }
        }
        Report rep;
        RC vr = s.verify(&rep);
        u32 bad = 0;
        for (int r = 0; r < 4; ++r)
            for (int i = 0; i < 200; ++i) {
                u32 x = 0;
                if (!get_i(s, (u64)(r * 1000 + i), x) || x != (u32)(r * 100 + i)) ++bad;
            }
        RC ro = s.open(&dev, arena, 256);
        u32 bad2 = 0;
        for (int r = 0; r < 4; ++r)
            for (int i = 0; i < 200; ++i) {
                u32 x = 0;
                if (!get_i(s, (u64)(r * 1000 + i), x) || x != (u32)(r * 100 + i)) ++bad2;
            }
        Report rep2;
        RC vr2 = s.verify(&rep2);
        std::printf("NOR: verify=%s bad=%u reopen=%s bad=%u verify2=%s cells=%u runs=%u\n",
                    rc_str(vr), bad, rc_str(ro), bad2, rc_str(vr2), rep2.cells, rep2.runs);
        if (vr != RC::OK || vr2 != RC::OK || bad || bad2) ++fails;
    }

    // ---------------------------------------------------------------- 2) 崩溃注入
    {
        FaultDev dev(4096);
        Store s;
        s.open(&dev, arena, 256);
        for (int i = 0; i < 400; ++i) put_i(s, (u64)(i + 1), (u32)(i * 3 + 1), 1);
        if (s.commit() != RC::OK) { std::printf("crash base commit\n"); return 1; }
        std::vector<u8> img = dev.buf;                 // 已提交基线镜像
        auto restore = [&]() { dev.buf = img; dev.wcount = 0; dev.fail_at = -1; };

        // 先跑一次完整提交，量出写次数
        Store t0;
        t0.open(&dev, arena, 256);
        for (int i = 0; i < 400; ++i) put_i(t0, (u64)(i + 1), (u32)(9000 + i), 2);
        t0.commit();
        u32 nw = (u32)dev.st.writes;
        restore();

        u32 trials = 0, okstate = 0;
        for (int cut = 1; cut <= (int)nw; ++cut) {
            restore();
            Store t;
            if (t.open(&dev, arena, 256) != RC::OK) { std::printf("crash open\n"); return 1; }
            for (int i = 0; i < 400; ++i) put_i(t, (u64)(i + 1), (u32)(9000 + i), 2);
            dev.wcount = 0;
            dev.fail_at = cut;                          // 第 cut 次写失败
            RC rc = t.commit();
            dev.fail_at = -1;
            ++trials;
            // 无论提交成功与否，重开都必须是完整状态：要么全旧，要么全新
            Store u;
            RC ro = u.open(&dev, arena, 256);
            u32 bad = 0;
            if (ro != RC::OK) ++bad;
            else {
                u32 oldv = 0, newv = 0;
                for (int i = 0; i < 400; ++i) {
                    u32 x = 0;
                    if (!get_i(u, (u64)(i + 1), x)) { ++bad; continue; }
                    if (x == (u32)(i * 3 + 1)) ++oldv;
                    else if (x == (u32)(9000 + i)) ++newv;
                    else ++bad;
                }
                if (oldv != 400 && newv != 400) ++bad;    // 混合 = 撕裂
                Report rr;
                if (u.verify(&rr) != RC::OK) ++bad;
                if (rc == RC::OK && newv != 400) ++bad;
                if (rc != RC::OK && oldv != 400 && newv != 400) ++bad;
            }
            if (!bad) ++okstate;
            else std::printf("  crash@%d rc=%s bad=%u\n", cut, rc_str(rc), bad);
        }
        std::printf("CRASH: writes/commit=%u trials=%u state_ok=%u\n", nw, trials, okstate);
        if (trials == 0 || okstate != trials) ++fails;
    }

    // ---------------------------------------------------------------- 3) 损坏检测
    {
        RamDev dev(2048);
        Store s;
        s.open(&dev, arena, 256);
        for (int i = 0; i < 500; ++i) put_i(s, (u64)(i + 1), (u32)(i + 7), 3);
        s.commit();
        Report rep;
        RC vr = s.verify(&rep);
        // 找到第一个数据页并翻转一位
        u32 victim = NIL;
        for (u32 p = 2; p < dev.capacity(); ++p) {
            const u8* q = dev.raw(p);
            if (rd32(q) == PK_DATA) { victim = p; break; }
        }
        u32 bad_get = 0, total = 0;
        if (victim != NIL) {
            dev.raw(victim)[16] ^= 0x01;            // 首键/记录区翻一位
            s.open(&dev, arena, 256);               // 冷缓存重开（模拟掉电后）
            Report rep2;
            RC vr2 = s.verify(&rep2);
            for (int i = 0; i < 500; ++i) {
                u8 b[32]; u16 l = 0;
                RC g = s.get((u64)(i + 1), b, sizeof b, &l);
                if (g == RC::OK) {
                    u32 x = 0;
                    std::memcpy(&x, b, 4);
                    ++total;
                    if (x != (u32)(i + 7)) ++bad_get;     // 读到错值 = 静默损坏
                }
            }
            std::printf("CORRUPT data: verify=%s (%s) readable=%u bad=%u page=%u\n",
                        rc_str(vr2), rep2.errors ? rep2.first_err : "-", total, bad_get, victim);
            if (vr2 != RC::CORRUPT) ++fails;
            if (bad_get) ++fails;
        } else {
            std::printf("CORRUPT data: no data page found\n");
            ++fails;
        }
        (void)vr;

        // 清单页损坏 → 重开必须报错而不是返回垃圾
        RamDev d2(2048);
        {
            Store t;
            t.open(&d2, arena, 256);
            // 连续提交，攒出一个真正的清单页（小增量内联在超级块里，不会立刻落页）
            for (int r = 0; r < 60; ++r) {
                for (int i = 0; i < 40; ++i) put_i(t, (u64)(r * 40 + i + 1), (u32)i, 4);
                t.commit();
            }
            u32 man = NIL;
            for (u32 p = 2; p < d2.capacity(); ++p)
                if (rd32(d2.raw(p)) == PK_MAN2 || rd32(d2.raw(p)) == PK_MAN) { man = p; break; }   // 第 16 轮：v2 清单页 'MAN2'（老库混链里仍可能有 'MANC'）
            if (man != NIL) {
                d2.raw(man)[20] ^= 0x01;
                Store u;
                RC ro = u.open(&d2, arena, 256);
                std::printf("CORRUPT manifest: reopen=%s\n", rc_str(ro));
                if (ro == RC::OK) ++fails;
            } else {
                std::printf("CORRUPT manifest: none found\n");
                ++fails;
            }
        }
    }

    // ---------------------------------------------------------------- 3) 异步写的崩溃点
    {
        FaultDev dev(4096);
        {
            Store s;
            if (s.open(&dev, arena, 256) != RC::OK) { std::printf("async open\n"); return 1; }
            s.set_async_writes(true);
            s.set_async_flush_min(64);
            for (int i = 0; i < 300; ++i) put_i(s, (u64)(i + 1), (u32)(i * 3 + 1), 1);
            if (s.commit() != RC::OK) { std::printf("async base commit\n"); return 1; }
        }
        // (a) 非持久批：commit(false) 已把页丢给后台线程 ⇒ 模拟"数据页落了、提交点没落"的掉电
        {
            Store s;
            s.open(&dev, arena, 256);
            s.set_async_writes(true);
            s.set_async_flush_min(64);
            for (int r = 0; r < 6; ++r) {
                for (int i = 0; i < 300; ++i) put_i(s, (u64)(i + 1), (u32)(9000 + r * 1000 + i), 2);
                s.commit(false);
            }
        }   // 析构 = 等后台线程收尾后"断电"
        {
            Store u;
            u32 bad = 0, oldv = 0, newv = 0;
            RC ro = u.open(&dev, arena, 256);
            if (ro != RC::OK) ++bad;
            else {
                for (int i = 0; i < 300; ++i) {
                    u32 x = 0;
                    if (!get_i(u, (u64)(i + 1), x)) { ++bad; continue; }
                    if (x == (u32)(i * 3 + 1)) ++oldv;
                    else ++newv;
                }
                Report rr;
                if (u.verify(&rr) != RC::OK) ++bad;
            }
            if (oldv != 300) ++bad;                 // 非持久提交可丢，但必须完整回落到旧态
            std::printf("ASYNC 掉电: rc=%s old=%u new=%u bad=%u\n", rc_str(ro), oldv, newv, bad);
            if (bad) ++fails;
        }
        // (b) 持久提交 + 异步：提交点之后一条都不能少
        {
            Store s;
            s.open(&dev, arena, 256);
            s.set_async_writes(true);
            s.set_async_flush_min(64);
            for (int i = 0; i < 300; ++i) put_i(s, (u64)(i + 1), (u32)(7000 + i), 3);
            if (s.commit() != RC::OK) { std::printf("async durable commit\n"); return 1; }
        }
        {
            Store u;
            u32 bad = 0;
            RC ro = u.open(&dev, arena, 256);
            if (ro != RC::OK) ++bad;
            else {
                for (int i = 0; i < 300; ++i) {
                    u32 x = 0;
                    if (!get_i(u, (u64)(i + 1), x) || x != (u32)(7000 + i)) ++bad;
                }
                Report rr;
                if (u.verify(&rr) != RC::OK) ++bad;
            }
            std::printf("ASYNC 持久: rc=%s bad=%u\n", rc_str(ro), bad);
            if (bad) ++fails;
        }
    }

    std::printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
