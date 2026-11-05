// 坑 156：dual_mem 完整消融探针
// 用法：
//   dual_probe write  <0|1 dual> [N=50000] [B=200] [VSZ=128] [soft=1] [async=0]
//   dual_probe lock   <0|1 dual> [N=20000] [B=500] [VSZ=512]     # 大 value：flush_excl 对照
//   dual_probe stall  <0|1 dual> [N=8000]  [B=400] [VSZ=256]     # 写中读者 get max
//   dual_probe a1     <0|1 dual> [N=50000] [B=500] [VSZ=32]      # Soft A1 峰（NOTE）
//   dual_probe async  <0|1 dual> [N=50000] [B=200] [VSZ=128]     # Soft async 写吞吐
//   dual_probe mw     <0|1 dual> [N=20000] [B=200] [VSZ=256] [W=3] # 多写者+1 读者
#define private public
#include "fcs.h"
#undef private
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

static void setup_soft(Store& s, bool dual, bool async) {
    s.configure_deploy(async);
    // 消融写路径：关 Post 债，避免整理税混入 dual 对照
    s.set_soft_cons(Store::SoftCons::Off);
    s.set_maint_mode(2);   // 多写者消融：提交后不跑维护，减交叉噪声
    s.set_dual_mem(dual);
    s.clear_flush_timers();
}

static int emit_line(const char* mode, bool dual, u64 wall, u64 insert_us_op, u64 max_commit,
                     u64 excl, u64 enc, u64 io_us, u64 settle_us, u64 flushes, u64 get_max_us,
                     u32 peak_tail, double peak_x) {
    std::printf("mode=%s arm=%s wall_us=%llu insert_us_op=%llu max_commit_us=%llu "
                "flush_excl_us=%llu encode_us=%llu io_us=%llu settle_us=%llu flushes=%llu "
                "get_max_us=%llu peak_tail=%u peak_x=%.3f\n",
                mode, dual ? "on" : "off",
                (unsigned long long)wall, (unsigned long long)insert_us_op,
                (unsigned long long)max_commit, (unsigned long long)excl,
                (unsigned long long)enc, (unsigned long long)io_us, (unsigned long long)settle_us,
                (unsigned long long)flushes, (unsigned long long)get_max_us, peak_tail, peak_x);
    return 0;
}

static int run_write(bool dual, u32 N, u32 B, u32 VSZ, bool soft, bool async, const char* mode_tag) {
    RamDev dev(1u << 22);
    std::vector<u8> arena((size_t)16384 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 16384) != RC::OK) return 1;
    if (soft) setup_soft(s, dual, async);
    else {
        s.set_async_writes(async);
        s.set_dual_mem(dual);
        s.clear_flush_timers();
    }
    std::vector<u8> v(VSZ, 5);
    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8ull) + 1;
    u64 max_c = 0;
    const u64 t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        if (s.put(keys[i], v.data(), (u16)VSZ) != RC::OK) return 2;
        if ((i + 1) % B == 0) {
            u64 c0 = now_us();
            if (s.commit() != RC::OK) return 2;
            u64 d = now_us() - c0;
            if (d > max_c) max_c = d;
        }
    }
    {
        u64 c0 = now_us();
        if (s.commit() != RC::OK) return 2;
        u64 d = now_us() - c0;
        if (d > max_c) max_c = d;
    }
    const u64 wall = now_us() - t0;
    u8 out[4096];
    u16 olen = 0;
    if (VSZ <= sizeof out && s.get(keys[0], out, (u16)VSZ, &olen) != RC::OK) return 3;
    Report rep;
    if (s.verify(&rep) != RC::OK || rep.errors) return 4;
    return emit_line(mode_tag, dual, wall, wall / N, max_c,
                     s.flush_excl_us(), s.dual_mem_encode_us(), s.dual_mem_io_us(),
                     s.dual_mem_settle_us(), s.dual_mem_flushes(), 0, s.used_pages(), 0.0);
}

static int run_lock(bool dual, u32 N, u32 B, u32 VSZ) {
    // Soft sync + 大 value：对照 flush_excl（门：on ≤ 0.5× off）
    return run_write(dual, N, B, VSZ, true, false, "lock");
}

static int run_stall(bool dual, u32 N, u32 B, u32 VSZ) {
    RamDev dev(1u << 22);
    std::vector<u8> arena((size_t)8192 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 8192) != RC::OK) return 1;
    setup_soft(s, dual, false);
    s.set_shared_reads(true);
    std::vector<u8> v(VSZ, 7);
    // 先种一批可读键
    const u32 seed = std::min(N, 2000u);
    for (u32 i = 0; i < seed; ++i) s.put(i + 1, v.data(), (u16)VSZ);
    if (s.commit() != RC::OK) return 2;

    std::atomic<bool> go{false}, done{false};
    std::atomic<u64> get_max{0}, gets{0};
    std::thread reader([&] {
        u8 buf[4096];
        u16 olen = 0;
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!done.load(std::memory_order_acquire)) {
            u64 k = (gets.load(std::memory_order_relaxed) % seed) + 1;
            u64 t0 = now_us();
            RC rc = s.get(k, buf, (u16)std::min<u32>(VSZ, (u32)sizeof buf), &olen);
            u64 dt = now_us() - t0;
            if (rc == RC::OK) {
                u64 cur = get_max.load(std::memory_order_relaxed);
                while (dt > cur && !get_max.compare_exchange_weak(cur, dt)) {}
            }
            gets.fetch_add(1, std::memory_order_relaxed);
        }
    });

    go.store(true, std::memory_order_release);
    u64 max_c = 0;
    const u64 t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        if (s.put(100000ull + i, v.data(), (u16)VSZ) != RC::OK) {
            done.store(true);
            reader.join();
            return 2;
        }
        if ((i + 1) % B == 0) {
            u64 c0 = now_us();
            if (s.commit() != RC::OK) {
                done.store(true);
                reader.join();
                return 2;
            }
            u64 d = now_us() - c0;
            if (d > max_c) max_c = d;
        }
    }
    {
        u64 c0 = now_us();
        s.commit();
        u64 d = now_us() - c0;
        if (d > max_c) max_c = d;
    }
    const u64 wall = now_us() - t0;
    done.store(true, std::memory_order_release);
    reader.join();
    return emit_line("stall", dual, wall, wall / N, max_c,
                     s.flush_excl_us(), s.dual_mem_encode_us(), s.dual_mem_io_us(),
                     s.dual_mem_settle_us(), s.dual_mem_flushes(), get_max.load(), s.used_pages(), 0.0);
}

// 多写者 + 1 读者：测 wall / get_max / flush_excl（坑 156 升默缺口）
static int run_mw(bool dual, u32 N, u32 B, u32 VSZ, u32 nwriters) {
    if (nwriters < 2) nwriters = 2;
    if (nwriters > 8) nwriters = 8;
    RamDev dev(1u << 22);
    std::vector<u8> arena((size_t)16384 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 16384) != RC::OK) return 1;
    setup_soft(s, dual, false);
    s.set_shared_reads(true);
    std::vector<u8> v(VSZ, 3);
    const u32 seed = 3000;
    for (u32 i = 0; i < seed; ++i) s.put(i + 1, v.data(), (u16)VSZ);
    if (s.commit() != RC::OK) return 2;

    std::atomic<bool> go{false}, done{false};
    std::atomic<u64> get_max{0}, err{0};
    std::atomic<u64> max_commit{0};
    std::thread reader([&] {
        u8 buf[4096];
        u16 olen = 0;
        u64 n = 0;
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!done.load(std::memory_order_acquire)) {
            u64 k = (n++ % seed) + 1;
            u64 t0 = now_us();
            RC rc = s.get(k, buf, (u16)std::min<u32>(VSZ, (u32)sizeof buf), &olen);
            u64 dt = now_us() - t0;
            if (rc != RC::OK) err.fetch_add(1, std::memory_order_relaxed);
            else {
                u64 cur = get_max.load(std::memory_order_relaxed);
                while (dt > cur && !get_max.compare_exchange_weak(cur, dt)) {}
            }
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(nwriters);
    go.store(true, std::memory_order_release);
    const u64 t0 = now_us();
    for (u32 w = 0; w < nwriters; ++w) {
        writers.emplace_back([&, w] {
            const u64 base = 1'000'000ull * (w + 1);
            for (u32 i = 0; i < N; ++i) {
                if (s.put(base + i, v.data(), (u16)VSZ) != RC::OK) {
                    err.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if ((i + 1) % B == 0) {
                    u64 c0 = now_us();
                    RC rc = s.commit();
                    u64 d = now_us() - c0;
                    if (rc != RC::OK && rc != RC::BUSY) err.fetch_add(1, std::memory_order_relaxed);
                    u64 cur = max_commit.load(std::memory_order_relaxed);
                    while (d > cur && !max_commit.compare_exchange_weak(cur, d)) {}
                }
            }
            u64 c0 = now_us();
            (void)s.commit();
            u64 d = now_us() - c0;
            u64 cur = max_commit.load(std::memory_order_relaxed);
            while (d > cur && !max_commit.compare_exchange_weak(cur, d)) {}
        });
    }
    for (auto& th : writers) th.join();
    const u64 wall = now_us() - t0;
    done.store(true, std::memory_order_release);
    reader.join();
    if (err.load()) {
        std::fprintf(stderr, "mw errors=%llu\n", (unsigned long long)err.load());
        return 5;
    }
    Report rep;
    if (s.verify(&rep) != RC::OK || rep.errors) return 4;
    const u64 ops = (u64)N * nwriters;
    return emit_line("mw", dual, wall, ops ? wall / ops : 0, max_commit.load(),
                     s.flush_excl_us(), s.dual_mem_encode_us(), s.dual_mem_io_us(),
                     s.dual_mem_settle_us(), s.dual_mem_flushes(), get_max.load(), s.used_pages(), 0.0);
}

static int run_a1(bool dual, u32 N, u32 B, u32 VSZ) {
    RamDev dev(1u << 22);
    std::vector<u8> arena((size_t)16384 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 16384) != RC::OK) return 1;
    setup_soft(s, dual, false);
    std::vector<u8> v(VSZ, 7);
    std::vector<u64> keys(N);
    for (u32 i = 0; i < N; ++i) keys[i] = mix(i + 1) % (N * 8ull) + 1;
    u32 peak = 0;
    const u64 t0 = now_us();
    for (u32 i = 0; i < N; ++i) {
        s.put(keys[i], v.data(), (u16)VSZ);
        if ((i + 1) % B == 0) {
            if (s.commit() != RC::OK) return 2;
            if (s.used_pages() > peak) peak = s.used_pages();
        }
    }
    if (s.commit() != RC::OK) return 2;
    if (s.used_pages() > peak) peak = s.used_pages();
    const u64 wall = now_us() - t0;
    u64 live = s.live_pages() ? s.live_pages() : 1;
    double peak_x = (double)peak / (double)live;
    return emit_line("a1", dual, wall, wall / N, 0,
                     s.flush_excl_us(), s.dual_mem_encode_us(), s.dual_mem_io_us(),
                     s.dual_mem_settle_us(), s.dual_mem_flushes(), 0, peak, peak_x);
}

// 慢 sync 窗口读者（g17 精神）：commit 睡在 fsync 时测 get_max
struct HookDev : RamDev {
    std::atomic<int> sync_delay_ms{0};
    std::atomic<int> in_sync{0};
    using RamDev::RamDev;
    void do_sync() override {
        in_sync.fetch_add(1);
        int d = sync_delay_ms.load();
        if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        in_sync.fetch_sub(1);
    }
    bool wait_in_sync(int tmo_ms = 3000) {
        for (int i = 0; i < tmo_ms && !in_sync.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return in_sync.load() > 0;
    }
};

static int run_syncstall(bool dual, u32 rounds, u32 VSZ) {
    HookDev dev(1u << 20);
    std::vector<u8> arena((size_t)4096 * PAGE);
    Store s;
    if (s.open(&dev, arena.data(), 4096) != RC::OK) return 1;
    setup_soft(s, dual, false);
    s.set_shared_reads(true);
    std::vector<u8> v(VSZ, 4);
    for (u32 i = 0; i < 2000; ++i) s.put(i + 1, v.data(), (u16)VSZ);
    if (s.commit() != RC::OK) return 2;
    u64 get_max = 0, wall = 0;
    const u64 t0 = now_us();
    dev.sync_delay_ms = 40;
    for (u32 r = 0; r < rounds; ++r) {
        u64 k = 500000ull + r;
        s.put(k, v.data(), (u16)VSZ);
        std::atomic<int> st{0};
        std::thread w([&] {
            st = 1;
            (void)s.commit();
            st = 2;
        });
        while (st.load() == 0) std::this_thread::yield();
        if (dev.wait_in_sync(2000) && st.load() == 1) {
            u8 buf[4096];
            u16 olen = 0;
            u64 g0 = now_us();
            (void)s.get(1, buf, (u16)std::min<u32>(VSZ, (u32)sizeof buf), &olen);
            u64 dt = now_us() - g0;
            if (dt > get_max) get_max = dt;
        }
        w.join();
    }
    wall = now_us() - t0;
    dev.sync_delay_ms = 0;
    return emit_line("syncstall", dual, wall, rounds ? wall / rounds : 0, 0,
                     s.flush_excl_us(), s.dual_mem_encode_us(), s.dual_mem_io_us(),
                     s.dual_mem_settle_us(), s.dual_mem_flushes(), get_max, s.used_pages(), 0.0);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: dual_probe <write|lock|stall|a1|async|mw|syncstall> <0|1> [N] [B] [VSZ] [W]\n");
        return 1;
    }
    const std::string mode = argv[1];
    const bool dual = argv[2][0] == '1';
    const u32 N = argc > 3 ? (u32)std::atoi(argv[3]) : 50000u;
    const u32 B = argc > 4 ? (u32)std::atoi(argv[4]) : 200u;
    const u32 VSZ = argc > 5 ? (u32)std::atoi(argv[5]) : 128u;
    const u32 W = argc > 6 ? (u32)std::atoi(argv[6]) : 3u;
    if (mode == "write") return run_write(dual, N, B, VSZ, true, false, "write");
    if (mode == "lock") return run_lock(dual, N, B ? B : 500u, VSZ ? VSZ : 512u);
    if (mode == "stall") return run_stall(dual, N ? N : 8000u, B ? B : 400u, VSZ ? VSZ : 256u);
    if (mode == "a1") return run_a1(dual, N, B ? B : 500u, VSZ ? VSZ : 32u);
    if (mode == "async") return run_write(dual, N, B, VSZ, true, true, "async");
    if (mode == "mw") return run_mw(dual, N ? N : 20000u, B ? B : 200u, VSZ ? VSZ : 256u, W);
    if (mode == "syncstall") return run_syncstall(dual, N ? N : 8u, VSZ ? VSZ : 120u);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
