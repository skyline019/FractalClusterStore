// debug trim_unit ownership after TRIM
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
    if (s.open(&dev, arena, 1024) != RC::OK) { puts("open fail"); return 1; }
    for (int b = 0; b < 8; ++b) {
        for (u64 k = (u64)b * 500; k < (u64)(b + 1) * 500; ++k) {
            u32 tag = 1; u8 v[200]; memset(v, 1, 200); memcpy(v, &tag, 4);
            s.put(k + 1, v, 200);
        }
        s.commit();
    }
    RunPtr old = nullptr;
    for (auto& kv : s.runs_) if (kv.second->npages >= 4) { old = kv.second; break; }
    if (!old) { puts("no run"); return 1; }
    s.ensure_meta(old);
    printf("old page0=%u np=%u mnpg=%u fences=%zu first=%llu\n",
           old->page0, old->npages, old->mnpg, old->fences.size(), (unsigned long long)old->first);
    for (u32 i = 0; i < std::min<u32>(4, (u32)old->fences.size()); ++i)
        printf("  fence[%u]=%llu\n", i, (unsigned long long)old->fences[i]);
    const u32 drop = 2;
    const u64 cover_hi = old->fences[drop];
    for (u64 k = old->first; k < cover_hi; ++k) {
        u32 tag = 2; u8 v[200]; memset(v, 2, 200); memcpy(v, &tag, 4);
        s.put(k, v, 200);
    }
    s.commit();
    printf("after cover: run still? %d page0=%u live_recs=%u pending_free=%zu free=%zu\n",
           (int)(s.runs_.count(old->page0)), old->page0, old->live_recs,
           s.pending_free_.size(), s.free_.size());
    Report r0; s.verify(&r0);
    printf("verify after cover: errors=%u first=%s\n", r0.errors, r0.first_err);

    const u32 old0 = old->page0, old_np = old->npages, mnpg = old->mnpg;
    std::vector<Store::ManEnt> delta;
    s.trim_run_prefix(old, drop, delta);
    printf("after trim: page0=%u np=%u mnpg=%u pending_free=%zu\n",
           old->page0, old->npages, old->mnpg, s.pending_free_.size());
    for (auto& e : s.pending_free_)
        printf("  pending [%u,+%u)\n", e.second, e.first);
    printf("  expected free prefix [%u,+%u) run now claims [%u,+%u)\n",
           old0, drop, old->page0, old->npages + old->mnpg);
    Report r1; s.verify(&r1);
    printf("verify after trim (pre-commit): errors=%u first=%s\n", r1.errors, r1.first_err);

    s.pend_delta_.insert(s.pend_delta_.end(), delta.begin(), delta.end());
    s.commit();
    printf("after commit: pending_free=%zu free=%zu sb_gen=%llu pend_need=%llu\n",
           s.pending_free_.size(), s.free_.size(),
           (unsigned long long)s.sb_gen_, (unsigned long long)s.pend_need_gen_);
    for (auto& kv : s.free_) printf("  free [%u,+%u)\n", kv.first, kv.second);
    Report r2; s.verify(&r2);
    printf("verify after commit: errors=%u first=%s\n", r2.errors, r2.first_err);
    return r2.errors ? 1 : 0;
}
