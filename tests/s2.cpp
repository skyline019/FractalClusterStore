// s2：长跑无漂移（soak）。
//     混合写/覆盖/删/异步组提交/快照回退/整库压实，跑很多轮之后：
//       页数、run 数、空闲碎片数、mem_bytes、存活页都必须有界且能回落，
//       绝不能单调增长；每一轮结束 verify 必须 errors=0；全程带看门狗。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <map>
#include <exception>
#include <chrono>
#include <vector>
using namespace fcs;

static u8 arena[512 * PAGE];
static int fails = 0;

static void onalarm(int) {
    const char msg[] = "HANG: 120 s 无进展 —— 长跑路径疑似死循环/死等\n";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    (void)r;
    _exit(9);
}
static void arm(int s) { signal(SIGALRM, onalarm); alarm((unsigned)s); }
static void disarm() { alarm(0); }

static Store* g_tr = nullptr;      // A4：第一次 CHECK 失败时把追踪环倒出来（最近 300 条分配/清单/提交事件）
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL %s\n", msg); if (++fails == 1 && g_tr) g_tr->trace_dump(stderr, 300); } } while (0)

static const u64 KS = 3000;                 // 键空间
static const int OPS = 40000;

struct Sample { int op; u32 used, runs, extents, live; u64 mem; };
static std::vector<Sample> g_samples;
static const char* g_stage = "start";
static int g_op = 0;

// 真值：key -> 值（空 = 不存在）
static std::map<u64, std::vector<u8>> truth, snap_truth;

static void resync(Store& s) {
    truth.clear();
    std::vector<u8> buf(PAGE);
    for (u64 k = 1; k <= KS; ++k) {
        u16 len = 0;
        RC rc = s.get(k, buf.data(), (u16)buf.size(), &len);
        if (rc == RC::OK) truth[k].assign(buf.begin(), buf.begin() + len);
    }
}

static void check_all(Store& s, const char* tag) {
    Report rep;
    RC vr = s.verify(&rep);
    if (vr != RC::OK || rep.errors)
        std::printf("    verify(%s) rc=%s errors=%u first=%s key=%llu\n", tag, rc_str(vr),
                    rep.errors, rep.first_err, (unsigned long long)rep.first_key);
    CHECK(vr == RC::OK && rep.errors == 0, tag);
    // 全量回读
    std::vector<u8> buf(PAGE);
    u32 bad = 0;
    for (u64 k = 1; k <= KS; ++k) {
        u16 len = 0;
        RC rc = s.get(k, buf.data(), (u16)buf.size(), &len);
        auto it = truth.find(k);
        bool has = it != truth.end();
        if (!has) { if (rc == RC::OK) ++bad; continue; }
        if (rc != RC::OK || len != it->second.size() ||
            (len && std::memcmp(buf.data(), it->second.data(), len) != 0)) {
            if (bad < 3)
                std::printf("    readback k=%llu rc=%s len=%u want=%zu\n", (unsigned long long)k,
                            rc_str(rc), (unsigned)len, it->second.size());
            ++bad;
        }
    }
    CHECK(bad == 0, "全量回读一致");
}

int main() {
    static RamDev dev(2048);                 // 8 MB：小到必须回收，大到放得下存活集
    Store s;
    g_tr = &s;
    CHECK(s.open(&dev, arena, 256) == RC::OK, "open");
    s.set_async_writes(true);
    s.set_io_threads(2);
    s.set_async_flush_min(16);
    s.set_key_index(true);

    std::set_terminate([] {
        std::fprintf(stderr, "TERMINATE at op=%d stage=%s\n", g_op, g_stage);
        std::abort();
    });
    u32 rs = 12345;
    auto rnd = [&]() { rs = rs * 1103515245u + 12345u; return rs >> 8; };
    std::vector<u8> v(1024);
    std::vector<double> seg_us;          // 每 4000 次操作的墙钟时间（查"长跑不减速"）
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto t_seg = now();
    int nsnap = 0;
    int nokcf = 0, last_cf_op = -1;
    u32 bad_prev = 0;
    arm(120);
    for (int i = 0; i < OPS; ++i) {
        s.trace_mark((u32)i);                       // A4：op 序号进追踪环，失败现场能直接对上 "op=NNNN"
        g_op = i;
        u32 r = rnd() % 100;
        u64 k = rnd() % KS + 1;
        g_stage = "put"; if (r < 62) {
            u32 len = 40 + rnd() % 560;
            for (u32 j = 0; j < len; ++j) v[j] = (u8)(j * 31 + i);
            RC rc = s.put(k, v.data(), (u16)len);
            if (rc == RC::OK) truth[k].assign(v.begin(), v.begin() + len);
        } else if (g_stage = "del", r < 74) {
            s.del(k);
            truth.erase(k);
        } else if (g_stage = "commit(false)", r < 82) {
            s.commit(false);                 // 逻辑提交（异步落盘）
        } else if (g_stage = "commit", r < 92) {
            RC rc = s.commit();
            CHECK(rc == RC::OK || rc == RC::NOSPACE, "commit");
        } else if (g_stage = "consolidate", r < 95) {
            RC rc = s.consolidate(0);        // 整库整流
            CHECK(rc == RC::OK || rc == RC::NOSPACE || rc == RC::BUSY, "consolidate");
        } else if (g_stage = "compact_file", r < 98) {
            RC rc = s.compact_file();        // 压实 + 收缩
            CHECK(rc == RC::OK || rc == RC::NOSPACE || rc == RC::BUSY, "compact_file");
            if (rc == RC::OK) last_cf_op = i;
            if (rc == RC::OK && nokcf++ < 8)
                std::printf("    compact_file OK op=%d used=%u live=%u runs=%u\n",
                            i, s.used_pages(), s.live_pages(), s.run_count());
        } else {
            g_stage = "snapshot/rollback";
            // 快照 = 持久提交点：snapshot() 内部先 commit()，所以快照内容 == 此刻的真值（含 mem_）。
            // 回退后**必须**精确回到快照时刻的真值 —— 以前这里只 resync 不比对，
            // 把三个回退 bug（坑 67/68/69）藏了两轮。
            if (nsnap == 0) { CHECK(s.snapshot() == RC::OK, "snapshot"); ++nsnap; snap_truth = truth; }
            else {
                CHECK(s.rollback(0) == RC::OK, "rollback"); --nsnap; resync(s);
                if (truth != snap_truth) {
                    u32 nd = 0;
                    for (auto& kv : snap_truth) { auto it = truth.find(kv.first); if (it == truth.end() || it->second != kv.second) { if (nd < 3) std::printf("    rollback mismatch k=%llu (%s)\n", (unsigned long long)kv.first, it == truth.end() ? "missing" : "value"); ++nd; } }
                    for (auto& kv : truth) if (!snap_truth.count(kv.first)) { if (nd < 3) std::printf("    rollback mismatch k=%llu (resurrected)\n", (unsigned long long)kv.first); ++nd; }
                    std::printf("    op=%d 回退后与快照真值不一致：%u 个键\n", i, nd);
                    CHECK(false, "回退精确回到快照时刻");
                }
            }
        }
        if (s.bad_manifest() != bad_prev) {
            std::printf("    op=%d 挂载丢弃坏清单项 %u（阶段=%s）\n", i,
                        s.bad_manifest() - bad_prev, g_stage);
            bad_prev = s.bad_manifest();
        }
        g_stage = "periodic commit";
        if ((i + 1) % 400 == 0) s.commit();
        if ((i + 1) % 4000 == 0) {
            seg_us.push_back(std::chrono::duration<double, std::micro>(now() - t_seg).count());
            t_seg = now();
            g_stage = "回落检查";
            // P5（第 9 轮）之后快照期的 compact_file 不再 BUSY，但只收"快照之后"的垃圾：老 run 原地不动（它可能钉着尾部，
            // s2 实测 used 575 / 实际占用 201），文件长度要等 forget 之后的下一次 compact_file 才回落到存活集。
            // 这里检查的是"没有快照时的回落性"，所以先 forget。
            if (nsnap) { s.forget_snapshots(); nsnap = 0; }
            RC crc = s.compact_file();
            if (crc == RC::BUSY) { s.forget_snapshots(); nsnap = 0; crc = s.compact_file(); }
            CHECK(crc == RC::OK, "compact_file 能把空间收回来");
            Sample sm{i + 1, s.used_pages(), s.run_count(), s.free_extents(), s.live_pages(), s.mem_bytes()};
            g_samples.push_back(sm);
            std::printf("  op=%6d used=%4u/%u runs=%3u extents=%3u live=%4u mem=%6lluKB cells=%u\n",
                        sm.op, sm.used, dev.max_pages, sm.runs, sm.extents, sm.live,
                        (unsigned long long)(sm.mem / 1024), s.cell_count());
            std::printf("          上次成功 compact_file op=%d\n", last_cf_op);
            g_stage = "check_all";
            check_all(s, "阶段性 verify");
            // 回落性：压实 + 收缩之后，占用必须回到存活集附近（允许 20% + 32 页余量）
            CHECK(sm.used <= sm.live * 6 / 5 + 32, "压实后回落到存活集");
            CHECK(sm.used < dev.max_pages * 3 / 4, "页数有界(不超设备上限)");
            CHECK(sm.runs < 64, "run 数有界");
            CHECK(sm.extents < 128, "空闲碎片有界");
            CHECK(sm.mem < (16u << 20), "mem_bytes 有界");
            CHECK(s.bad_manifest() == 0, "清单链完整（没有坏清单项）");
        }
    }
    RC rc = s.commit();
    CHECK(rc == RC::OK, "收尾提交");
    disarm();
    check_all(s, "收尾 verify");

    // 不漂移：垃圾量（used - live）在整个长跑里都必须有界，且首尾同阶。
    if (g_samples.size() >= 3) {
        const Sample& first = g_samples.front();
        const Sample& last = g_samples.back();
        u32 amp0 = first.used > first.live ? first.used - first.live : 0;
        u32 ampN = last.used > last.live ? last.used - last.live : 0;
        std::printf("  漂移检查: used %u -> %u, live %u -> %u, 垃圾 %u -> %u, runs %u -> %u, extents %u -> %u\n",
                    first.used, last.used, first.live, last.live, amp0, ampN,
                    first.runs, last.runs, first.extents, last.extents);
        CHECK(ampN <= amp0 + last.live / 2 + 160, "垃圾量不单调增长");
        CHECK(last.runs <= 64 && last.extents < 128, "结构数不单调增长");
    }
    // 不减速：最后一段的耗时不该比第一段差太多（出现 O(n²) 会立刻炸出来）
    if (seg_us.size() >= 4) {
        double first = seg_us.front(), last = seg_us.back();
        std::printf("  每 4k 次操作耗时: 首段 %.1f ms → 末段 %.1f ms（%.2fx）\n",
                    first / 1000, last / 1000, last / (first > 1 ? first : 1));
        CHECK(last < first * 10 + 500000, "长跑不减速（无 O(n²)）");
    }
    if (fails) { std::printf("FAILURES: %d\n", fails); return 1; }
    std::printf("PASS\n");
    return 0;
}
