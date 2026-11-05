#include "include/fcs.h"
#include <cstdio>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}
int main() {
    const u32 N = 20000, B = 500, VSZ = 32;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 9);
    const char* path = "/home/user/.fcs_oracle164.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 8 * 1024 * 1024) / PAGE + 4096));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    auto scan = [&](bool oneseg) {
        s.set_range_oneseg_sel(oneseg);
        u64 cnt = 0, x = 0;
        s.select(0, IDX_BASE, [&](u64 k, const u8* v, u16 l) {
            ++cnt; x ^= k; x ^= l; if (l) x ^= v[0];
            return true;
        }, 0);
        return std::make_pair(cnt, x);
    };
    auto a = scan(true), b = scan(false);
    // short windows
    u64 ca = 0, cb = 0, xa = 0, xb = 0;
    s.set_range_oneseg_sel(true);
    for (u32 i = 0; i < 500; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        s.select(lo, lo + 4096, [&](u64 k, const u8* v, u16 l) {
            ++ca; xa ^= k + l + (l ? v[0] : 0); return true;
        }, 100);
    }
    s.set_range_oneseg_sel(false);
    for (u32 i = 0; i < 500; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n];
        s.select(lo, lo + 4096, [&](u64 k, const u8* v, u16 l) {
            ++cb; xb ^= k + l + (l ? v[0] : 0); return true;
        }, 100);
    }
    // get oneseg vs multi (same)
    u8 buf[64]; u16 l = 0; u32 bad = 0;
    for (u32 i = 0; i < n; ++i) {
        if (s.get(keys[i], buf, sizeof buf, &l) != RC::OK || l != VSZ || buf[0] != 9) ++bad;
    }
    std::printf("full oneseg cnt=%llu x=%llu cursor cnt=%llu x=%llu\n",
                (unsigned long long)a.first, (unsigned long long)a.second,
                (unsigned long long)b.first, (unsigned long long)b.second);
    std::printf("short oneseg cnt=%llu x=%llu cursor cnt=%llu x=%llu get_bad=%u\n",
                (unsigned long long)ca, (unsigned long long)xa,
                (unsigned long long)cb, (unsigned long long)xb, bad);
    bool ok = a == b && ca == cb && xa == xb && bad == 0;
    std::printf("%s\n", ok ? "ORACLE164_OK" : "ORACLE164_FAIL");
    return ok ? 0 : 1;
}
