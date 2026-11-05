#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}
static double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(steady_clock::now().time_since_epoch()).count();
}
int main() {
    const u32 N = 50000, B = 500, VSZ = 32, S = 5000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    std::remove("/home/user/.fcs_o167.db");
    FileDev dev("/home/user/.fcs_o167.db", (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);

    // dump first run meta
    {
        Store::RGuard g(&s);
        // use public-ish: run_count only; print via select one
    }
    for (u32 i = 0; i < 2000; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
    }

    // A: select as-is
    auto med = [&](auto fn) {
        double a[5];
        for (int r = 0; r < 5; ++r) a[r] = fn();
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (a[j] < a[i]) { double t = a[i]; a[i] = a[j]; a[j] = t; }
        return a[2];
    };
    auto sel = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n], c = 0;
            s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++c; return true; }, K);
        }
        return (now_us() - t0) / S;
    };
    // B: get of same key (exact)
    u8 b[64]; u16 l = 0;
    auto gt = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i)
            s.get(keys[mix(i * 13 + 5) % n], b, sizeof b, &l);
        return (now_us() - t0) / S;
    };
    // C: lower_bound in keys then get — oracle for "find first in window"
    auto lb_get = [&] {
        double t0 = now_us();
        for (u32 i = 0; i < S; ++i) {
            u64 lo = keys[mix(i * 13 + 5) % n];
            auto it = std::lower_bound(keys.begin(), keys.end(), lo);
            if (it != keys.end() && *it < lo + 4096)
                s.get(*it, b, sizeof b, &l);
        }
        return (now_us() - t0) / S;
    };
    std::printf("med select=%.3f get=%.3f lb+get=%.3f\n", med(sel), med(gt), med(lb_get));

    // D: oneseg on/off short again (post-166 next-fast)
    double on[5], off[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_oneseg_sel(true);
        // temporarily need short oneseg — can't without code change; measure Cursor path variants
        on[r] = sel();
        s.set_range_oneseg_sel(false);
        off[r] = sel();
    }
    auto m5 = [](double* a) {
        double b[5] = {a[0], a[1], a[2], a[3], a[4]};
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
        return b[2];
    };
    std::printf("oneseg flag (limit!=0 no-op) on/off=%.3f\n", m5(on) / m5(off));

    // E: open_fast / page_view ablate short
    double of_on[5], of_off[5], pv_on[5], pv_off[5];
    for (int r = 0; r < 5; ++r) {
        s.set_range_open_fast(true); of_on[r] = sel();
        s.set_range_open_fast(false); of_off[r] = sel();
        s.set_range_open_fast(true);
        s.set_range_page_view(true); pv_on[r] = sel();
        s.set_range_page_view(false); pv_off[r] = sel();
        s.set_range_page_view(true);
    }
    std::printf("open_fast on/off=%.3f page_view on/off=%.3f\n",
                m5(of_on) / m5(of_off), m5(pv_on) / m5(pv_off));
    return 0;
}
