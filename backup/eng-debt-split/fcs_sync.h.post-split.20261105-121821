#pragma once
#include "fcs_codec.h"

namespace fcs {

// =============================================================================
//  M06  线程槽 / 读写锁  —  P1 读扩展性：TlsId · XLock
// =============================================================================
// 每个线程在进程范围内领一个槽号（0..MAX_TLS-1），线程退出时归还。Store 按槽号保存该线程的私有状态：
// 风险指针（正在使用的页缓存槽）、共享锁深度、每线程计数器、私有页缓冲 / 溢出链暂存。
// 领不到槽（同时活着的线程 > MAX_TLS）的线程仍然可用：读也走独占锁，用第 MAX_TLS 号"公共槽"。
static constexpr u32 MAX_TLS = 32;
struct TlsIdPool {
    std::mutex mu;
    u32 free_[MAX_TLS];
    u32 nfree;
    std::atomic<u32> hwm{0};         // 用过的最大槽号 + 1：扫风险指针只扫到这里（2 个线程就扫 2 格，不扫 32 格）
    TlsIdPool() : nfree(MAX_TLS) { for (u32 i = 0; i < MAX_TLS; ++i) free_[i] = MAX_TLS - 1 - i; }   // 先发小号
    static TlsIdPool& inst() { static TlsIdPool p; return p; }
    u32 acquire() {
        std::lock_guard<std::mutex> g(mu);
        if (!nfree) return MAX_TLS;
        const u32 id = free_[--nfree];
        if (id + 1 > hwm.load(std::memory_order_relaxed)) hwm.store(id + 1, std::memory_order_release);
        return id;
    }
    void release(u32 id) { if (id >= MAX_TLS) return; std::lock_guard<std::mutex> g(mu); free_[nfree++] = id; }
};
struct TlsId {
    u32 id;
    TlsId() : id(TlsIdPool::inst().acquire()) {}
    ~TlsId() { TlsIdPool::inst().release(id); }
};
inline u32 tls_id() { static thread_local TlsId t; return t.id; }

// 写侧可重入独占 + 读侧共享，**写者优先**。独占持有者线程再进任何入口（读或写）都是空操作。
// 坑 86（g7 `3 6000 4` 实抓，4 读者 2 核跑 15 分钟不结束）：std::shared_mutex 在 glibc 上是读者优先的 rwlock，
// 读者只要前赴后继地 lock_shared，写者可以**永远**拿不到锁 —— 2 读者时碰巧能插进去，4 读者就饿死。
// 自己写：有写者在等（wwait_ > 0）时新读者退让；最后一个读者出门叫醒写者；写者出门叫醒所有人。
class XLock {
public:
    static void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
    }
    void lock() {
        const auto me = std::this_thread::get_id();
        if (owner_.load(std::memory_order_relaxed) == me) { ++depth_; return; }
        // 让一手：有读者还在睡（上一个写者刚走、它们没来得及醒），先等它们拿到共享锁再举牌，**有上界**。
        // 否则写者连续插队（每次 unlock → notify → 读者醒来 50 µs 内写者又 lock 了）会把读者饿死：
        // g7 的写者满负荷时读者 10 s 只做了 314 次 get。代价：有读者等着时每次写入口多 ≤200 µs。
        if (rwait_.load(std::memory_order_seq_cst) > 0) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::microseconds(200), [&] { return rwait_.load(std::memory_order_seq_cst) == 0; });
        }
        wwait_.fetch_add(1, std::memory_order_seq_cst);        // 先举牌（与读者的 st_ 写 / wwait_ 读构成 Dekker 配对）
        int e = 0;
        if (!st_.compare_exchange_strong(e, -1, std::memory_order_seq_cst)) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { int x = 0; return st_.compare_exchange_strong(x, -1, std::memory_order_seq_cst); });
        }
        wwait_.fetch_sub(1, std::memory_order_seq_cst);
        owner_.store(me, std::memory_order_relaxed);
        depth_ = 1;
    }
    void unlock() {
        if (--depth_ != 0) return;
        owner_.store(std::thread::id(), std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(mu_); st_.store(0, std::memory_order_seq_cst); }   // 在 mu_ 内改：不丢唤醒
        cv_.notify_all();
    }
    bool held_by_me() const { return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id(); }
    // 第 14 轮：有没有人在门口等（读者在自旋 / 睡觉，或别的写者举了牌）。持锁的维护单元在单元边界之内（单元格边界 / 每个补算单元）
    // 看一眼，有人等就提前收工让路 —— 读者 / 提交线程的等待上界从"一片 256 页的归并"变成"一个单元格"。
    bool contended() const { return rwant_.load(std::memory_order_relaxed) > 0 || wwait_.load(std::memory_order_relaxed) > 0; }
    void lock_shared() {
        bool slept = false, want = false;
        for (unsigned spins = 0;; ++spins) {
            if (wwait_.load(std::memory_order_seq_cst) == 0) {
                int x = st_.load(std::memory_order_relaxed);
                if (x >= 0) {
                    if (st_.compare_exchange_weak(x, x + 1, std::memory_order_seq_cst)) {
                        if (want) rwant_.fetch_sub(1, std::memory_order_seq_cst);
                        if (slept && rwait_.fetch_sub(1, std::memory_order_seq_cst) == 1) {   // 最后一个睡过的读者进来了：放写者举牌
                            std::lock_guard<std::mutex> g(mu_);
                            cv_.notify_all();
                        }
                        return;
                    }
                    continue;                                   // 和别的读者撞了：直接重试
                }
                // x < 0：写者正持有。第 14 轮之前这里直接 continue —— 读者在写者持锁期间**空转**（2 核沙箱上烧掉持锁者的 CPU），
                // 现在和"有写者在等"一样：先让、再睡（写者 unlock 总会 notify_all）。
            }
            if (!want) { want = true; rwant_.fetch_add(1, std::memory_order_seq_cst); }
            if (spins < 200) { cpu_relax(); continue; }                 // 短持有（put / 一个单元格）：原地等几 µs，不切线程
            if (spins < 200 + 64) { std::this_thread::yield(); continue; }
            std::unique_lock<std::mutex> lk(mu_);
            if (!slept) { slept = true; rwait_.fetch_add(1, std::memory_order_seq_cst); }
            cv_.wait(lk, [&] { return wwait_.load(std::memory_order_seq_cst) == 0 && st_.load(std::memory_order_seq_cst) >= 0; });
            spins = 0;
        }
    }
    void unlock_shared() {
        if (st_.fetch_sub(1, std::memory_order_seq_cst) == 1 && wwait_.load(std::memory_order_seq_cst) != 0) {
            std::lock_guard<std::mutex> lk(mu_);                // 配合写者在 mu_ 内评估谓词：不丢唤醒
            cv_.notify_all();
        }
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<int> st_{0};         // >0 读者数；-1 写者；0 空闲
    std::atomic<unsigned> wwait_{0}; // 正在等/正在拿的写者数
    std::atomic<unsigned> rwait_{0}; // 睡过一觉、还没拿到共享锁的读者数（写者让手的依据）
    std::atomic<unsigned> rwant_{0}; // 第 14 轮：正在等共享锁的读者数（含自旋中的）——维护单元让路的依据
    std::atomic<std::thread::id> owner_{};
    u32 depth_ = 0;
};

// ============================================================================

} // namespace fcs
