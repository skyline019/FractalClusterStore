// r2：空间耗尽与"绝不挂死"。每个阶段都带 15 s 看门狗：
//     正常路径必须返回明确的 RC（而非转圈），并且陈旧空间要能被回收后继续写。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
using namespace fcs;

static u8 arena[256 * PAGE];
static int fails = 0;

// 可控失败的设备：arm 之后下一笔写返回 IO（模拟坏块/掉线）
struct FaultDev : RamDev {
    explicit FaultDev(PageId n) : RamDev(n) {}
    int fail_in = -1;
    void arm_fail_next_write() { fail_in = 0; }
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        if (fail_in == 0) { fail_in = -1; return RC::IO; }
        if (fail_in > 0) --fail_in;
        return RamDev::do_write(pid, off, src, len);
    }
};

// 慢设备：写会真的阻塞（模拟 NAND program 卡住 / 网络块设备掉队）。
// 只要 set_io_timeout_ms 生效，调用方就必须拿到 RC::TIMEOUT 而不是永远等下去。
struct SlowDev : RamDev {
    explicit SlowDev(PageId n) : RamDev(n) {}
    std::atomic<int> delay_ms{0};
    RC do_write(PageId pid, u32 off, const void* src, u32 len) override {
        int d = delay_ms.load();
        if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        return RamDev::do_write(pid, off, src, len);
    }
};

static void onalarm(int) {
    const char msg[] = "HANG: 15 s 无进展 —— 疑似死循环/死锁\n";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    (void)r;
    _exit(9);
}
static void arm(int s = 15) { signal(SIGALRM, onalarm); alarm((unsigned)s); }
static void disarm() { alarm(0); }
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL %s\n", msg); ++fails; } } while (0)

// 阶段 1：小设备真的装不下 ⇒ 必须返回 NOSPACE（不是死循环），verify 干净；
//         删掉一部分键之后 compact 要能把空间收回来，库要能重新长回来。
static void phase_full() {
    std::printf("-- 灌满小设备（RamDev 96 页）\n");
    static RamDev dev(96);
    Store s;
    CHECK(s.open(&dev, arena, 128) == RC::OK, "open");
    std::vector<u8> v(700, 0x33);
    RC last = RC::OK;
    int n = 0;
    arm();
    for (; n < 200000; ++n) {
        RC rc = s.put((u64)n + 1, v.data(), 700);      // 全是新键：存活集一定超过设备
        if (rc != RC::OK) { last = rc; break; }
        if (n % 20 == 19) { rc = s.commit(); if (rc != RC::OK) { last = rc; break; } }
    }
    if (last == RC::OK) last = s.commit();
    disarm();
    std::printf("  停在第 %d 次写，RC=%s（%u 页）\n", n, rc_str(last), s.used_pages());
    CHECK(last == RC::NOSPACE || last == RC::FULL, "灌满时应返回 NOSPACE/FULL");
    Report rep;
    arm(); RC vr = s.verify(&rep); disarm();
    if (vr != RC::OK || rep.errors)
        std::printf("    verify rc=%s errors=%u first=%s key=%llu\n", rc_str(vr), rep.errors,
                    rep.first_err, (unsigned long long)rep.first_key);
    CHECK(vr == RC::OK && rep.errors == 0, "灌满后 verify");

    // 只保留 1/4 的键：腾出来的是"真存活空间"，compact 必须能成功
    arm();
    for (int i = n / 4; i < n; ++i) s.del((u64)i + 1);
    RC rc = s.commit();
    disarm();
    if (rc != RC::OK) std::printf("    del commit rc=%s（%u 页）\n", rc_str(rc), s.used_pages());
    CHECK(rc == RC::OK, "删除后提交");
    arm(); rc = s.compact_file(); disarm();
    std::printf("  compact_file: %s（%u 页）\n", rc_str(rc), s.used_pages());
    CHECK(rc == RC::OK || rc == RC::NOSPACE, "compact_file 不得挂死/崩溃");
    // shrink 之后必须还能长回来（历史 bug：截短一次就永久 NOSPACE）
    arm();
    int wrote = 0;
    for (int i = 0; i < 200; ++i) {
        if (s.put((u64)i + 100000, v.data(), 700) != RC::OK) break;
        if (s.commit() == RC::OK) ++wrote;
    }
    disarm();
    std::printf("  回收后又写了 %d 次（%u 页）\n", wrote, s.used_pages());
    CHECK(wrote > 20, "回收/收缩后应能继续写");
    arm(); vr = s.verify(&rep); disarm();
    CHECK(vr == RC::OK && rep.errors == 0, "回收后 verify");
}

// 阶段 2：异步写 + 灌满 ⇒ 提交点不能等一个永远不会完成的页
static void phase_async_full() {
    std::printf("-- 异步写 + 灌满（RamDev 128 页，2 线程）\n");
    static RamDev dev(128);
    Store s;
    CHECK(s.open(&dev, arena, 128) == RC::OK, "open");
    s.set_async_writes(true);
    s.set_io_threads(2);
    s.set_async_flush_min(8);                 // 每次逻辑提交都真的落一段
    std::vector<u8> v(900, 0x5a);
    RC last = RC::OK;
    int n = 0;
    arm();
    for (; n < 200000; ++n) {
        RC rc = s.put((u64)n + 1, v.data(), 900);
        if (rc != RC::OK) { last = rc; break; }
        s.commit(false);
        if (n % 25 == 24) { rc = s.commit(); if (rc != RC::OK) { last = rc; break; } }
    }
    if (last == RC::OK) last = s.commit();
    disarm();
    std::printf("  停在第 %d 次写，RC=%s（%u 页，后台完成 %llu 次页写）\n", n, rc_str(last),
                s.used_pages(), (unsigned long long)s.io_done());
    CHECK(last == RC::NOSPACE || last == RC::FULL, "异步灌满时应返回 NOSPACE/FULL");
    Report rep;
    arm(); RC vr = s.verify(&rep); disarm();
    if (vr != RC::OK || rep.errors)
        std::printf("    verify rc=%s errors=%u first=%s key=%llu\n", rc_str(vr), rep.errors,
                    rep.first_err, (unsigned long long)rep.first_key);
    CHECK(vr == RC::OK && rep.errors == 0, "异步灌满后 verify");
    arm(); RC rc = s.compact_file(); disarm();
    std::printf("  compact_file: %s（%u 页）\n", rc_str(rc), s.used_pages());
    CHECK(rc == RC::OK || rc == RC::NOSPACE, "异步 compact_file 不得挂死");
}

// 阶段 3：写失败注入 ⇒ 失败必须上抛；故障消失后提交要能恢复，verify 干净
static void phase_io_faults() {
    std::printf("-- 写失败注入（异步，每 7 次提交坏一笔写）\n");
    static FaultDev dev(512);
    Store s;
    CHECK(s.open(&dev, arena, 128) == RC::OK, "open");
    s.set_async_writes(true);
    s.set_io_threads(2);
    s.set_async_flush_min(8);
    std::vector<u8> v(300, 0x11);
    u32 errors = 0, ok = 0;
    arm();
    for (int i = 0; i < 400; ++i) {
        if (i % 7 == 3) dev.arm_fail_next_write();
        s.put((u64)(i % 30) + 1, v.data(), 300);
        RC rc = s.commit();
        if (rc == RC::OK) ++ok; else ++errors;
    }
    dev.fail_in = -1;                          // 故障消失
    RC rc = RC::IO;
    for (int i = 0; i < 4 && rc != RC::OK; ++i) rc = s.commit();   // 应当自愈
    disarm();
    std::printf("  提交成功 %u 次、失败 %u 次；恢复提交：%s\n", ok, errors, rc_str(rc));
    CHECK(ok > 0 && errors > 0, "故障注入下应既有成功也有失败（且不挂死）");
    CHECK(rc == RC::OK, "故障消失后提交应恢复");
    Report rep;
    arm(); RC vr = s.verify(&rep); disarm();
    CHECK(vr == RC::OK && rep.errors == 0, "故障注入后 verify");
}

// 阶段 4：回退必须持久 —— 重挂载后看到的仍是回退后的状态。
// （旧实现的回退只把旧状态写进一个超级块槽，另一个槽里"版本更高的旧分支"
//   会在下次挂载时胜出，把回退整条复活。）
static void phase_rollback_remount() {
    std::printf("-- 回退后重挂载（逻辑时钟必须单调）\n");
    static RamDev dev(256);
    std::vector<u8> a(120, 0xAA), b(120, 0xBB), c(120, 0xCC);
    auto getc = [](Store& s, u64 k, u8* out) -> int {
        u16 len = 0;
        RC rc = s.get(k, out, PAGE, &len);
        return rc == RC::OK ? (int)out[0] : -1;
    };
    u8 buf[PAGE];
    arm();
    {
        Store s;
        CHECK(s.open(&dev, arena, 128) == RC::OK, "open");
        s.put(1, a.data(), (u16)a.size());
        s.put(2, a.data(), (u16)a.size());
        CHECK(s.commit() == RC::OK, "commit1");
        CHECK(s.snapshot() == RC::OK, "snapshot");
        s.put(1, b.data(), (u16)b.size());
        s.del(2);
        s.put(3, c.data(), (u16)c.size());
        CHECK(s.commit() == RC::OK, "commit2");
        CHECK(getc(s, 1, buf) == 0xBB && getc(s, 3, buf) == 0xCC, "回退前是新状态");
        CHECK(s.rollback(0) == RC::OK, "rollback");
        CHECK(getc(s, 1, buf) == 0xAA, "回退后 key1 是旧值");
        CHECK(getc(s, 2, buf) == 0xAA, "回退后 key2 复活");
        CHECK(getc(s, 3, buf) == -1, "回退后 key3 消失");
        Report rep;
        RC vr = s.verify(&rep);
        CHECK(vr == RC::OK && rep.errors == 0, "回退后 verify");
    }
    {   // 重挂载：盘上的提交点必须是"回退后的状态"
        Store s2;
        CHECK(s2.open(&dev, arena, 128) == RC::OK, "重新 open");
        CHECK(getc(s2, 1, buf) == 0xAA, "重挂载后 key1 仍是旧值");
        CHECK(getc(s2, 2, buf) == 0xAA, "重挂载后 key2 仍存在");
        CHECK(getc(s2, 3, buf) == -1, "重挂载后 key3 仍不存在");
        Report rep;
        RC vr = s2.verify(&rep);
        if (vr != RC::OK || rep.errors)
            std::printf("    verify rc=%s errors=%u first=%s\n", rc_str(vr), rep.errors, rep.first_err);
        CHECK(vr == RC::OK && rep.errors == 0, "重挂载后 verify");
    }
    disarm();
}

// 阶段 5：设备卡住 ⇒ 有界等待必须报 TIMEOUT（不许挂死），故障解除后自愈
static void phase_stuck_device() {
    std::printf("-- 慢设备（写卡 3 s）+ 有界等待 150 ms\n");
    static SlowDev dev(256);
    Store s;
    CHECK(s.open(&dev, arena, 128) == RC::OK, "open");
    s.set_async_writes(true);
    s.set_io_threads(1);
    s.set_async_flush_min(8);
    s.set_io_timeout_ms(150);
    std::vector<u8> v(400, 0x77);
    for (int i = 0; i < 40; ++i) s.put((u64)i + 1, v.data(), 400);
    CHECK(s.commit() == RC::OK, "正常提交");
    dev.delay_ms.store(3000);                 // 设备突然变慢
    arm(20);                                  // 看门狗：真挂住 20 s 就报出来
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) s.put((u64)i + 1, v.data(), 400);
    RC rc = s.commit();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    disarm();
    std::printf("  卡住时的提交：%s（%lld ms，超时计数 %llu）\n", rc_str(rc), (long long)ms,
                (unsigned long long)s.io_timeouts());
    CHECK(rc == RC::TIMEOUT, "设备卡住时必须返回 TIMEOUT（不能挂死/不能谎报成功）");
    CHECK(ms < 3000, "必须在有界时间内返回");
    dev.delay_ms.store(0);                    // 设备恢复
    s.set_io_timeout_ms(0);                   // 这一笔要等它把卡住的那页写完
    RC rc2 = RC::TIMEOUT;
    arm(30);
    for (int i = 0; i < 5 && rc2 != RC::OK; ++i) { s.put(1, v.data(), 400); rc2 = s.commit(); }
    disarm();
    std::printf("  恢复后提交：%s\n", rc_str(rc2));
    CHECK(rc2 == RC::OK, "故障解除后提交自愈");
    Report rep;
    arm();
    RC vr = s.verify(&rep);
    disarm();
    if (vr != RC::OK || rep.errors)
        std::printf("    verify rc=%s errors=%u first=%s\n", rc_str(vr), rep.errors, rep.first_err);
    CHECK(vr == RC::OK && rep.errors == 0, "卡住恢复后 verify");
}

int main() {
    phase_full();
    phase_async_full();
    phase_io_faults();
    phase_rollback_remount();
    phase_stuck_device();
    if (fails) { std::printf("FAILURES: %d\n", fails); return 1; }
    std::printf("PASS\n");
    return 0;
}
