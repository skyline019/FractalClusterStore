#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#define private public
#include "fcs.h"
#undef private
using namespace fcs;

int main() {
    RamDev dev(8192);
    static u8 arena[1024 * PAGE];
    Store s;
    s.set_maint_mode(2);
    s.set_lvl_t(1000);
    s.open(&dev, arena, 1024);
    for (int b = 0; b < 8; ++b) {
        for (u64 k = (u64)b * 500; k < (u64)(b + 1) * 500; ++k) {
            u32 tag = 1; u8 v[200]; memset(v, 1, 200); memcpy(v, &tag, 4);
            s.put(k + 1, v, 200);
        }
        s.commit();
    }
    RunPtr old = nullptr;
    for (auto& kv : s.runs_) if (kv.second->npages >= 4) { old = kv.second; break; }
    s.ensure_meta(old);
    const u32 drop = 2;
    const u64 cover_hi = old->fences[drop];
    for (u64 k = old->first; k < cover_hi; ++k) {
        u32 tag = 2; u8 v[200]; memset(v, 2, 200); memcpy(v, &tag, 4);
        s.put(k, v, 200);
    }
    s.commit();
    printf("tail=%u runs=%zu\n", s.tail_, s.runs_.size());
    for (auto& kv : s.runs_) {
        auto* r = kv.second;
        s.ensure_meta(r);
        printf("  run p0=%u np=%u mnpg=%u aux=%u blk=%u first=%llu\n",
               r->page0, r->npages, r->mnpg, r->aux_npg, Store::run_block(r),
               (unsigned long long)r->first);
    }
    std::vector<Store::ManEnt> delta;
    s.trim_run_prefix(old, drop, delta);
    printf("AFTER TRIM tail=%u\n", s.tail_);
    for (auto& kv : s.runs_) {
        auto* r = kv.second;
        printf("  run p0=%u np=%u mnpg=%u blk=%u\n", r->page0, r->npages, r->mnpg, Store::run_block(r));
    }
    for (auto& e : s.pending_free_) printf("  pending [%u,+%u)\n", e.second, e.first);
    for (auto& e : s.deferred_) printf("  deferred [%u,+%u)\n", e.second, e.first);
    for (auto& kv : s.free_) printf("  free [%u,+%u)\n", kv.first, kv.second);
    printf("man_last=%u nman=%u\n", s.man_last_, s.nman_);

    // manual ownership scan
    std::vector<u8> own(s.tail_, 0);
    auto claim = [&](u32 p0, u32 n, u8 k) {
        for (u32 i = 0; i < n; ++i) {
            u32 p = p0 + i;
            if (p >= s.tail_) { printf("  BEYOND %u kind=%u\n", p, k); continue; }
            if (own[p]) printf("  TWICE page=%u was=%u now=%u\n", p, own[p], k);
            else own[p] = k;
        }
    };
    for (auto& kv : s.runs_) claim(kv.second->page0, Store::run_block(kv.second), 1);
    for (auto& e : s.pending_free_) claim(e.second, e.first, 6);
    for (auto& e : s.deferred_) claim(e.second, e.first, 5);
    for (auto& kv : s.free_) claim(kv.first, kv.second, 4);
    if (s.man_last_ != NIL) {
        u32 pid = s.man_last_;
        for (u32 i = 0; i < s.nman_ && pid != NIL; ++i) {
            claim(pid, 1, 3);
            const u8* p = s.page(pid);
            pid = p ? rd32(p + 4) : (u32)NIL;
        }
    }
    for (u32 p = 2; p < s.tail_; ++p)
        if (!own[p]) printf("  UNOWNED %u\n", p);
    return 0;
}
