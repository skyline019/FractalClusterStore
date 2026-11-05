// 坑 158 探针：Soft sync flush 分项 — excl / settle（经典路径 settle 在 flush_excl 内）
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
    const u32 N = 50000, B = 500, VSZ = 32;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7), val2(VSZ, 3);
    const char* path = "/home/user/.fcs_flush158.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.clear_flush_timers();
    double t0 = now_us();
    for (u32 i = 0; i < n; ++i) {
        if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) return 2;
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double ins = (now_us() - t0) / n;
    // update half
    s.clear_flush_timers();
    t0 = now_us();
    for (u32 i = 0; i < n / 2; ++i) {
        s.put(keys[mix(i + 999) % n], val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double upd = (now_us() - t0) / (n / 2);
    std::printf("soft_sync insert=%.3f excl_us=%llu dual_settle=%llu (classic settle folded into excl)\n",
                ins, (unsigned long long)s.flush_excl_us(),
                (unsigned long long)s.dual_mem_settle_us());
    std::printf("soft_sync update=%.3f excl_us=%llu\n", upd, (unsigned long long)s.flush_excl_us());
    // dual compare
    std::remove(path);
    FileDev d2(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s2;
    s2.open(&d2, nullptr, 2048);
    s2.set_tidy_policy(Store::TidyPolicy::Soft);
    s2.set_dual_mem(true);
    s2.clear_flush_timers();
    t0 = now_us();
    for (u32 i = 0; i < n; ++i) {
        s2.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s2.commit();
    }
    s2.commit();
    double dins = (now_us() - t0) / n;
    std::printf("dual_on insert=%.3f excl=%llu enc=%llu io=%llu settle=%llu flushes=%llu\n",
                dins,
                (unsigned long long)s2.flush_excl_us(),
                (unsigned long long)s2.dual_mem_encode_us(),
                (unsigned long long)s2.dual_mem_io_us(),
                (unsigned long long)s2.dual_mem_settle_us(),
                (unsigned long long)s2.dual_mem_flushes());
    return 0;
}
