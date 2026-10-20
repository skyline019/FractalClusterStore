// XDB v2 —— 块设备适配器与写放大计量
// 设备只保证「4 字节对齐的粒度写入」：非对齐写由适配器读-改-写放大（并计入字节）。
//
// 并发约定：设备必须支持「不同页上的并发读/写」（引擎的异步提交会多线程写页）。
// 同页的并发访问由引擎自己排除（异步写在入队时保证同页的先后顺序）。
#pragma once
#include "common.h"
#include <atomic>
#include <cstdio>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>          // fallocate / FALLOC_FL_PUNCH_HOLE（R8 局部 TRIM）
#endif
#include <cstdlib>
#include <vector>

namespace xdb {

struct DevStats {
    std::atomic<u64> writes{0};        // 写的**页数**（write_multi 一次写 n 页记 n；写放大口径不变）
    std::atomic<u64> write_calls{0};   // 写**调用**次数（FileDev = 系统调用数；B4 的度量）
    std::atomic<u64> write_bytes{0};   // 实际写字节
    std::atomic<u64> read_calls{0};
    std::atomic<u64> read_bytes{0};
    std::atomic<u64> erases{0};
    std::atomic<u64> erase_bytes{0};
    std::atomic<u64> rmw{0};           // 触发的读-改-写次数
    DevStats() = default;
    void reset() {
        writes = 0; write_calls = 0; write_bytes = 0; read_calls = 0; read_bytes = 0;
        erases = 0; erase_bytes = 0; rmw = 0;
    }
};

struct BlockDev {
    u32 page = PAGE;
    DevStats st;
    virtual ~BlockDev() {}

    RC read(PageId pid, u32 off, void* dst, u32 len) {
        RC rc = do_read(pid, off, dst, len);
        if (rc == RC::OK) { st.read_calls.fetch_add(1, std::memory_order_relaxed); st.read_bytes.fetch_add(len, std::memory_order_relaxed); }
        return rc;
    }
    RC write(PageId pid, u32 off, const void* src, u32 len) {
        RC rc = do_write(pid, off, src, len);
        if (rc == RC::OK) {
            st.writes.fetch_add(1, std::memory_order_relaxed);
            st.write_calls.fetch_add(1, std::memory_order_relaxed);
            st.write_bytes.fetch_add(len, std::memory_order_relaxed);
        }
        return rc;
    }
    // 连续 n 整页一次写（B4）。默认逐页转调 do_write —— 故障注入/掉电模拟的设备仍然看到每一页；
    // FileDev / RamDev 覆盖成一次 pwrite / memcpy。统计口径：writes 记 n 页，write_calls 记实际调用数。
    RC write_multi(PageId pid, u32 n, const void* src) {
        if (n == 0) return RC::OK;
        u32 calls = n;
        RC rc = do_write_multi(pid, n, src, calls);
        if (rc == RC::OK) {
            st.writes.fetch_add(n, std::memory_order_relaxed);
            st.write_calls.fetch_add(calls, std::memory_order_relaxed);
            st.write_bytes.fetch_add((u64)n * page, std::memory_order_relaxed);
        }
        return rc;
    }
    // 连续多页一次读。默认逐页转调 read()；FileDev 覆盖成一次 pread
    //（少 n-1 次系统调用 —— 顺序扫描时这是主要成本）。
    virtual RC read_multi(PageId pid, u32 n, void* dst) {
        u8* d = (u8*)dst;
        for (u32 i = 0; i < n; ++i) if (read(pid + i, 0, d + (size_t)i * page, page) != RC::OK) return RC::IO;
        return RC::OK;
    }
    RC erase(PageId pid) {
        RC rc = do_erase(pid);
        if (rc == RC::OK) { st.erases.fetch_add(1, std::memory_order_relaxed); st.erase_bytes.fetch_add(page, std::memory_order_relaxed); }
        return rc;
    }
    void sync() { do_sync(); }

    virtual PageId capacity() const = 0;
    // 设备偏好"整页写"吗？（B4）
    //   文件/块设备：true —— 页缓存本来就按 4 KB 页脏，部分页写反而可能触发读-改-写；连续整页能一次 pwrite。
    //   NOR / 字节可寻址（RamDev 作为它们的模拟）：false —— 只写用到的字节是真实的磨损/时间节省，
    //   引擎保持"按用量逐页写"（写放大口径也不变）。
    virtual bool whole_pages() const { return false; }
    // 调整设备长度：可减可增，但不超过创建时声明的大小 max_pages。
    // 增长是为了让 shrink() 之后的库还能继续写（截短只是还空间，不是封顶）。
    bool truncate(PageId np) {
        if (max_pages && np > max_pages) return false;
        if (!do_truncate(np)) return false;
        return true;
    }
    PageId max_pages = 0;          // 设备上限（构造时设定）
protected:
    virtual RC do_read(PageId pid, u32 off, void* dst, u32 len) = 0;
    virtual RC do_write(PageId pid, u32 off, const void* src, u32 len) = 0;
    virtual RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) {
        calls = n;
        for (u32 i = 0; i < n; ++i)
            if (do_write(pid + i, 0, (const u8*)src + (size_t)i * page, page) != RC::OK) return RC::IO;
        return RC::OK;
    }
    virtual RC do_erase(PageId pid) { (void)pid; return RC::OK; }
    virtual void do_sync() {}
    virtual bool do_truncate(PageId) { return false; }
public:
    // R8（第 11 轮）局部 TRIM：告诉设备 [pid, pid+n) 这段页**现在没人引用、随后会被重写**，设备可以把它变成洞
    //（FileDev：fallocate PUNCH_HOLE，稀疏文件省磁盘；SSD 上由文件系统转成 discard）。语义上等价于"允许返回任意内容"，
    // 所以调用方只能对"已经过了提交点、本来就可以复用"的页集合调用（Store::promote_pending 之后的 free_）。
    // 默认什么都不做；失败也无所谓（纯优化）。
    virtual void discard(PageId pid, u32 n) { (void)pid; (void)n; }
};

// ------------------------------------------------------------------ RAM 设备
class RamDev : public BlockDev {
public:
    RamDev(PageId npages, u32 psize = PAGE) : np_(npages), ps_(psize) {
        max_pages = npages;
        buf_.assign((size_t)npages * psize, 0);
    }
    RC do_read(PageId pid, u32 off, void* dst, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        std::memcpy(dst, &buf_[(size_t)pid * ps_ + off], len);
        return RC::OK;
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        std::memcpy(&buf_[(size_t)pid * ps_ + off], src, len);
        return RC::OK;
    }
    RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) override {
        if ((u64)pid + n > np_) return RC::IO;
        std::memcpy(&buf_[(size_t)pid * ps_], src, (size_t)n * ps_);
        calls = 1;
        return RC::OK;
    }
    PageId capacity() const override { return np_; }
    const u8* raw(PageId pid) const { return &buf_[(size_t)pid * ps_]; }
    u8* raw(PageId pid) { return &buf_[(size_t)pid * ps_]; }
protected:
    bool do_truncate(PageId np) override {
        buf_.resize((size_t)np * ps_, 0);
        np_ = np;
        return true;
    }
private:
    bool valid(PageId pid, u32 off, u32 len) const {
        return pid < np_ && off + len <= ps_ && (off % 4 == 0) && (len % 4 == 0);
    }
    std::vector<u8> buf_;
    std::atomic<PageId> np_;      // 异步写线程在 valid() 里读、主线程在 truncate 里改：原子即可（容量不会超过构造时的 buf_，resize 不搬内存）
    u32 ps_;
};

// ------------------------------------------------------------------ 文件设备
// pread/pwrite：无共享文件位置 ⇒ 不同页的并发读写天然安全（多写线程的前提）。
class FileDev : public BlockDev {
public:
    FileDev(const char* path, PageId npages, u32 psize = PAGE, bool create = true)
        : np_(npages), ps_(psize) {
        max_pages = npages;
        f_ = std::fopen(path, create ? "r+b" : "rb");
        if (!f_ && create) {
            f_ = std::fopen(path, "w+b");
            if (f_) {
                // 用 ftruncate 建稀疏文件：比写一整片 0 快几个数量级，
                // 也不会让第一次 fsync 去刷几 MB 的"脏零页"
                if (::ftruncate(fileno(f_), (off_t)((u64)npages * psize)) != 0) { /* 交给后续 IO 报错 */ }
                std::fflush(f_);
            }
        }
        if (f_) fd_ = ::fileno(f_);
    }
    ~FileDev() override { if (f_) std::fclose(f_); }
    bool ok() const { return f_ != nullptr; }
    void discard(PageId pid, u32 n) override {
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
        if (fd_ < 0 || n == 0) return;
        (void)::fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)((u64)pid * ps_), (off_t)((u64)n * ps_));
        ++n_discard_;
#else
        (void)pid; (void)n;
#endif
    }
    u64 discards() const { return n_discard_; }

    RC do_read(PageId pid, u32 off, void* dst, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(fd_, (u8*)dst + got, len - got, (long)((size_t)pid * ps_ + off + got));
            if (n <= 0) return RC::IO;
            got += (size_t)n;
        }
        return RC::OK;
    }
    RC read_multi(PageId pid, u32 n, void* dst) override {
        if (pid + n > np_) return RC::IO;
        size_t want = (size_t)n * ps_, got = 0;
        while (got < want) {
            ssize_t r = ::pread(fd_, (u8*)dst + got, want - got, (long)((size_t)pid * ps_ + got));
            if (r <= 0) return RC::IO;
            got += (size_t)r;
        }
        st.read_calls.fetch_add(1, std::memory_order_relaxed);
        st.read_bytes.fetch_add((u64)want, std::memory_order_relaxed);
        return RC::OK;
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        size_t put = 0;
        while (put < len) {
            ssize_t n = ::pwrite(fd_, (const u8*)src + put, len - put, (long)((size_t)pid * ps_ + off + put));
            if (n <= 0) return RC::IO;
            put += (size_t)n;
        }
        return RC::OK;
    }
    bool whole_pages() const override { return true; }
    RC do_write_multi(PageId pid, u32 n, const void* src, u32& calls) override {
        if ((u64)pid + n > np_) return RC::IO;
        size_t want = (size_t)n * ps_, put = 0;
        calls = 0;
        while (put < want) {
            ssize_t w = ::pwrite(fd_, (const u8*)src + put, want - put, (long)((size_t)pid * ps_ + put));
            if (w <= 0) return RC::IO;
            put += (size_t)w;
            ++calls;
        }
        return RC::OK;
    }
    PageId capacity() const override { return np_; }
    void do_sync() override {
        if (f_) { std::fflush(f_); ::fdatasync(fd_); }
    }
    u64 file_bytes() const { return (u64)np_ * ps_; }
protected:
    bool do_truncate(PageId np) override {
        if (!f_ || ::ftruncate(fd_, (off_t)((u64)np * ps_)) != 0) return false;
        std::fflush(f_);
        np_ = np;
        return true;
    }
private:
    bool valid(PageId pid, u32 off, u32 len) const {
        return pid < np_ && off + len <= ps_ && (off % 4 == 0) && (len % 4 == 0);
    }
    FILE* f_ = nullptr;
    int fd_ = -1;
    u64 n_discard_ = 0;
    PageId np_;
    u32 ps_;
};

// ------------------------------------------------------------------ NOR 模拟
// 页粒度擦除；写入只允许 1→0（NOR 语义）；支持在下次写注入 I/O 故障。
class NorSimDev : public BlockDev {
public:
    NorSimDev(PageId npages, u32 psize = PAGE) : np_(npages), ps_(psize) {
        max_pages = npages;
        buf_.assign((size_t)npages * psize, 0xFF);
    }
    RC do_read(PageId pid, u32 off, void* dst, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        std::memcpy(dst, &buf_[(size_t)pid * ps_ + off], len);
        return RC::OK;
    }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (!valid(pid, off, len)) return RC::IO;
        if (arm_fail_) { arm_fail_ = false; return RC::IO; }      // 崩溃注入
        const u8* s = (const u8*)src;
        u8* d = &buf_[(size_t)pid * ps_ + off];
        for (u32 i = 0; i < len; ++i) d[i] &= s[i];               // NOR 只能 1→0
        return RC::OK;
    }
    RC do_erase(PageId pid) override {
        if (pid >= np_) return RC::IO;
        std::memset(&buf_[(size_t)pid * ps_], 0xFF, ps_);
        return RC::OK;
    }
    PageId capacity() const override { return np_; }
    void arm_fail_next_write() { arm_fail_ = true; }
    const u8* raw(PageId pid) const { return &buf_[(size_t)pid * ps_]; }
private:
    bool valid(PageId pid, u32 off, u32 len) const {
        return pid < np_ && off + len <= ps_ && (off % 4 == 0) && (len % 4 == 0);
    }
    std::vector<u8> buf_;
    PageId np_;
    u32 ps_;
    bool arm_fail_ = false;
};

} // namespace xdb
