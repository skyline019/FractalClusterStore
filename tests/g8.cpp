// g8 —— 覆盖风暴 × 多单元 × 层合并 × 快照钉住（R0 护栏；坑 89/90/91 回归）
//
// 第 10 轮修悬崖时发现：老引擎在"几千个键、几百次提交、每次覆盖几百个键"的工况下**读到过期值**
// （bench6 ovw 5000 500 200 200：22% 的键回读是旧版本），g5 的工况（几十个键、交错操作）从未走到这个状态。
// 机制（坑 90）：压实产物沿用最新源的 seq，之后进入层合并，合并产物取组内最大 seq = 那条还活着的刷盘 run 的 seq，
// 再靠 rid 决胜 ⇒ 对该刷盘 run 仍最新的键，合并产物以更老的版本胜出。
// 本测试做的事：
//   1) nkeys 个键、每次提交覆盖 per 个随机键、commits 次提交（随机值长，含短值和溢出值），
//      **每次提交后逐键全量比对模型**（值前 4 字节 = 最后一次写入的序号）；首个出错的提交号会打印出来；
//   2) 周期性 snapshot()，持有期间抽样用 get_at(k, 快照版本) 比对快照时刻的模型副本（坑 91：被钉住的最老 run
//      不能和更新的 run 跨过中间 run 组队），再 forget_snapshots()；
//   3) 结束：verify()（含 run refs / run lvl 核对），run 数、单元最大段数、层号直方图的上界；
//   4) 析构再 open 同一设备（重开），再全量比对 + verify()。
// 用法：build/g8 [seed] [nkeys] [commits] [per]；make test 跑 3 组（短值 / 混合 / 多键）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <random>
using namespace fcs;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (fails <= 10) { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

struct Model { std::vector<u32> tag; std::vector<u16> len; };

static void mkval(std::vector<u8>& v, u64 k, u32 tag, u16 len) {
    v.resize(len);
    std::memcpy(v.data(), &tag, 4);
    for (size_t i = 4; i < v.size(); ++i) v[i] = (u8)(k * 31 + i * 7 + tag * 131);
}
static bool val_ok(const u8* p, u16 len, u64 k, u32 tag, u16 want_len) {
    if (len != want_len || len < 4) return false;
    u32 t = 0; std::memcpy(&t, p, 4);
    if (t != tag) return false;
    for (size_t i = 4; i < len; ++i) if (p[i] != (u8)(k * 31 + i * 7 + tag * 131)) return false;
    return true;
}

// 逐键比对；返回坏键数，第一个坏键写入 *first
static u32 compare_all(Store& s, const Model& m, u32 nkeys, u32 view, u64* first, u32* first_tag) {
    std::vector<u8> buf(16384);
    u32 bad = 0;
    for (u64 k = 1; k <= nkeys; ++k) {
        u16 len = 0;
        RC rc = view ? s.get_at(k, view, buf.data(), (u16)buf.size(), &len)
                     : s.get(k, buf.data(), (u16)buf.size(), &len);
        bool ok;
        if (!m.tag[k]) ok = (rc == RC::NOTFOUND);
        else ok = (rc == RC::OK) && val_ok(buf.data(), len, k, m.tag[k], m.len[k]);
        if (!ok) {
            if (!bad) {
                *first = k;
                u32 t = 0; if (rc == RC::OK && len >= 4) std::memcpy(&t, buf.data(), 4);
                *first_tag = t;
            }
            ++bad;
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    int seed = argc > 1 ? std::atoi(argv[1]) : 1;
    u32 nkeys = argc > 2 ? (u32)std::atoi(argv[2]) : 4000;
    u32 commits = argc > 3 ? (u32)std::atoi(argv[3]) : 160;
    u32 per = argc > 4 ? (u32)std::atoi(argv[4]) : 200;
    const u32 arena_pages = 512;
    std::vector<u8> arena((size_t)arena_pages * PAGE);
    // 设备别开太大：RamDev 构造时就把整块内存清零（300000 页 = 1.2 GB，ASan 下直接把沙箱吃干）。
    // 默认工况高水位 ~6000 页，8000 键 × 400/提交 ~12000 页；给 32768 页（128 MB）够用且 ASan 下安全。
    RamDev dev(32768);
    Store* s = new Store();
    if (s->open(&dev, arena.data(), arena_pages) != RC::OK) { std::printf("open fail\n"); return 1; }

    std::mt19937 rng(seed);
    Model m; m.tag.assign(nkeys + 1, 0); m.len.assign(nkeys + 1, 0);
    Model snap_m; u32 snap_v = 0; bool snap_held = false; u32 snap_at = 0;
    std::vector<u8> v;
    u32 tag = 0, n_snap = 0, n_snap_cmp = 0, first_bad_commit = 0;
    u32 max_runs = 0, max_segs = 0, max_used = 0;

    for (u32 c = 1; c <= commits; ++c) {
        for (u32 i = 0; i < per; ++i) {
            u64 k = rng() % nkeys + 1;
            ++tag;
            u32 r = rng() % 100;
            u16 len = r < 70 ? (u16)(16 + rng() % 200)           // 短值
                    : r < 95 ? (u16)(200 + rng() % 1200)          // 中值
                             : (u16)(2000 + rng() % 6000);        // 溢出值
            if (r == 99) { s->del(k); m.tag[k] = 0; m.len[k] = 0; continue; }
            mkval(v, k, tag, len);
            RC rc = s->put(k, v.data(), len);
            CHECK(rc == RC::OK, "put rc=%d at commit %u", (int)rc, c);
            m.tag[k] = tag; m.len[k] = len;
        }
        RC rc = s->commit();
        CHECK(rc == RC::OK, "commit %u rc=%d", c, (int)rc);
        max_runs = std::max(max_runs, s->run_count());
        max_used = std::max(max_used, s->used_pages());
        max_segs = std::max(max_segs, s->max_segs_per_cell());

        // 1) 当前视图全量比对
        u64 fk = 0; u32 ft = 0;
        u32 bad = compare_all(*s, m, nkeys, 0, &fk, &ft);
        if (bad && !first_bad_commit) first_bad_commit = c;
        CHECK(bad == 0, "commit %u: %u keys stale/wrong; first key %llu got tag %u want %u (runs=%u)",
              c, bad, (unsigned long long)fk, ft, m.tag[fk], s->run_count());

        // 2) 快照：每 25 次提交抓一个、持有 12 次提交后释放；持有期间比对快照视图
        if (!snap_held && c % 25 == 5) {
            if (s->snapshot() == RC::OK) { snap_held = true; snap_m = m; snap_v = s->last_commit(); snap_at = c; ++n_snap; }
        } else if (snap_held) {
            u64 sk = 0; u32 st = 0;
            u32 sbad = compare_all(*s, snap_m, nkeys, snap_v, &sk, &st);
            CHECK(sbad == 0, "commit %u: snapshot(at %u) view %u keys wrong; first key %llu got %u want %u",
                  c, snap_at, sbad, (unsigned long long)sk, st, snap_m.tag[sk]);
            ++n_snap_cmp;
            if (c - snap_at >= 12) { s->forget_snapshots(); snap_held = false; }
        }
        if (fails > 10) break;
    }
    if (snap_held) { s->forget_snapshots(); snap_held = false; }

    // 3) 末态核对
    Report rep; RC vr = s->verify(&rep);
    CHECK(vr == RC::OK && rep.errors == 0, "verify rc=%d errors=%u first=%s key=%llu", (int)vr, rep.errors, rep.first_err, (unsigned long long)rep.first_key);
    std::vector<u32> h = s->lvl_hist();
    u32 top = h.empty() ? 0 : h.back();
    std::printf("seed=%d nkeys=%u commits=%u per=%u: runs=%u (max %u) cells=%u max_segs=%u (max %u) lvl=", seed, nkeys, commits, per,
                s->run_count(), max_runs, s->cell_count(), s->max_segs_per_cell(), max_segs);
    for (size_t i = 0; i < h.size(); ++i) std::printf("%s%u", i ? "/" : "", h[i]);
    std::printf(" snaps=%u snap_cmps=%u used_max=%u\n", n_snap, n_snap_cmp, max_used);
    // run 数上界：L0 最多 4×lvl_t(32) 条 + 其它层各 ≤ 32 + 顶层；经验值 ≤ 96（老引擎同工况 300+）
    CHECK(s->run_count() <= 96, "too many runs: %u（层号/合并路径回归？）", s->run_count());
    CHECK(max_segs <= 48, "max segs per cell %u（压实没跟上？）", max_segs);
    CHECK(top <= 8, "top level has %u runs（本该只由 L3 合并/提升产生）", top);

    // 4) 重开
    delete s;
    s = new Store();
    if (s->open(&dev, arena.data(), arena_pages) != RC::OK) { std::printf("reopen fail\n"); return 1; }
    u64 fk = 0; u32 ft = 0;
    u32 bad = compare_all(*s, m, nkeys, 0, &fk, &ft);
    CHECK(bad == 0, "after reopen: %u keys wrong; first key %llu got %u want %u", bad, (unsigned long long)fk, ft, m.tag[fk]);
    Report rep2; vr = s->verify(&rep2);
    CHECK(vr == RC::OK && rep2.errors == 0, "verify after reopen rc=%d errors=%u first=%s", (int)vr, rep2.errors, rep2.first_err);
    // 重开后再跑一段，确认合并路径对挂载来的 run 同样成立
    for (u32 c = 1; c <= 20; ++c) {
        for (u32 i = 0; i < per; ++i) {
            u64 k = rng() % nkeys + 1; ++tag; u16 len = (u16)(16 + rng() % 300);
            mkval(v, k, tag, len); s->put(k, v.data(), len); m.tag[k] = tag; m.len[k] = len;
        }
        s->commit();
        bad = compare_all(*s, m, nkeys, 0, &fk, &ft);
        CHECK(bad == 0, "post-reopen commit %u: %u keys wrong; first key %llu got %u want %u", c, bad, (unsigned long long)fk, ft, m.tag[fk]);
        if (fails > 10) break;
    }
    delete s;
    if (first_bad_commit) std::printf("  first bad commit: %u\n", first_bad_commit);
    std::printf("%s (fails=%d)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
