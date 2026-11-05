// 坑 156 S9：settle 热点粗分
#define private public
#include "fcs.h"
#undef private
#include <chrono>
#include <cstdio>
#include <vector>
using namespace fcs;

static u64 now_us() {
    using namespace std::chrono;
    return (u64)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}

int main() {
    RamDev dev(1u << 22);
    std::vector<u8> arena((size_t)16384 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 16384) != RC::OK) return 1;
    s.configure_deploy(false);
    s.set_soft_cons(Store::SoftCons::Off);
    s.set_maint_mode(2);
    s.set_dual_mem(true);
    const u32 N = 50000, B = 200, VSZ = 128;
    std::vector<u8> v(VSZ, 5);
    for (u32 i = 0; i < N; ++i) {
        if (s.put(mix(i + 1) % (N * 8ull) + 1, v.data(), (u16)VSZ) != RC::OK) return 2;
        if ((i + 1) % B == 0 && s.commit() != RC::OK) return 2;
    }
    if (s.commit() != RC::OK) return 2;
    s.wait_settle_idle();
    std::printf("flushes=%llu settle_us=%llu excl=%llu enc=%llu io=%llu cells=%zu runs=%zu\n",
                (unsigned long long)s.dual_mem_flushes(),
                (unsigned long long)s.dual_mem_settle_us(),
                (unsigned long long)s.flush_excl_us(),
                (unsigned long long)s.dual_mem_encode_us(),
                (unsigned long long)s.dual_mem_io_us(),
                s.cells_.size(), s.runs_.size());

    // 再测：对已有键 probe_prev 吞吐
    std::vector<u64> keys(200);
    for (u32 i = 0; i < 200; ++i) keys[i] = mix(i + 1) % (N * 8ull) + 1;
    u64 t0 = now_us();
    u32 hits = 0;
    for (int rep = 0; rep < 250; ++rep) {
        for (u64 k : keys) {
            u16 len = 0; u32 osz = 0; RunPtr own = nullptr;
            if (s.probe_prev(k, s.open_vers(), len, &own, &osz)) ++hits;
        }
    }
    u64 dt = now_us() - t0;
    std::printf("probe_prev 250x200: us=%llu hits=%u us/key=%.2f\n",
                (unsigned long long)dt, hits, (double)dt / (250.0 * 200.0));
    return 0;
}
