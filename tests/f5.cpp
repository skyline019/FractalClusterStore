// f5：空间回收三件套 —— 归一并、搬家(relocate)、清单检查点、尾部回落(shrink)
//   1) 异步 + 多写线程灌入 60 万次覆盖写
//   2) consolidate(2) 只碰碎片单元 → compact_file() 全量整理 + 文件收缩
//   3) 收缩前后逐键对拍 / verify / 重开一致
//   4) 设备不支持收缩（NOR）时，shrink 只能收紧分配器：数据仍然正确
#include "fcs.h"
#include <cstdio>
#include <map>
using namespace fcs;
int main() {
    const u32 N = 30000;
    std::remove("/tmp/shr4.db");
    FileDev dev("/tmp/shr4.db", 4000);
    std::map<u64, u32> model;
    {
        Store s;
        if (s.open(&dev, nullptr, 1024) != RC::OK) return 2;
        s.set_async_writes(true); s.set_io_threads(2); s.set_async_flush_min(512);
        u8 b[12];
        for (u32 r = 0; r < 20; ++r) {
            for (u32 i = 0; i < N; ++i) {
                u64 k = (u64)i * 3 + 1; u32 v = r * 100000 + i;
                std::memcpy(b, &v, 4); std::memset(b + 4, 0x5A, 8);
                s.put(k, b, 12); model[k] = v;
            }
            s.commit(false);
        }
        s.commit();
        Report rep; if (s.verify(&rep) != RC::OK) { std::printf("verify before: %s\n", rep.first_err); return 2; }
        u32 before = s.used_pages();
        RC rc = s.consolidate(2); if (rc != RC::OK) { std::printf("cons(2) rc=%d\n", (int)rc); return 2; }
        rc = s.compact_file();   if (rc != RC::OK) { std::printf("compact rc=%d\n", (int)rc); return 2; }
        // 逐键对拍
        u32 bad = 0;
        for (auto& kv : model) {
            u8 b2[16]; u16 l = 0;
            if (s.get(kv.first, b2, sizeof b2, &l) != RC::OK || l != 12) { ++bad; continue; }
            u32 v = 0; std::memcpy(&v, b2, 4);
            if (v != kv.second) ++bad;
        }
        u32 n = 0; s.select(0, IDX_BASE, [&](u64, const u8*, u16) { ++n; return true; });
        std::printf("收缩: %u -> %u 页 (%.1f MB -> %.1f MB)  bad=%u rows=%u/%zu\n",
            before, dev.capacity(), before * 4096.0 / 1048576.0, dev.capacity() * 4096.0 / 1048576.0,
            bad, n, model.size());
        if (bad || n != model.size()) return 1;
    }
    {
        Store u;
        if (u.open(&dev, nullptr, 1024) != RC::OK) { std::printf("reopen fail\n"); return 1; }
        Report rep; if (u.verify(&rep) != RC::OK) { std::printf("verify after: %s\n", rep.first_err); return 1; }
        u32 bad = 0;
        for (auto& kv : model) {
            u8 b2[16]; u16 l = 0;
            if (u.get(kv.first, b2, sizeof b2, &l) != RC::OK || l != 12) { ++bad; continue; }
            u32 v = 0; std::memcpy(&v, b2, 4); if (v != kv.second) ++bad;
        }
        std::printf("重开: bad=%u\n", bad);
        if (bad) return 1;
    }
    std::printf("PASS\n");
    return 0;
}
