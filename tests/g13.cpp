// g13 —— 第 13 轮回归（EVAL12 §6 的五个问题）：
//   ② 归并产物流式落盘（R12）：预留 → 分块落页 → 原地收尾 / 估小搬家 / 预留拿不到就退回老路径；sync + async × RamDev + FileDev；
//      快照钉住 / 回退 / 重开；小设备 NOSPACE 回退
//   ① （第 16 轮 P2 改写）seq / ver 64 位：预置到 2^32 之上跨界提交 / 快照 / 重开不截断；v1 清单链：测试侧降级器造老库 ⇒
//      只读挂载不写回 → 写即升级 v2（链里老页照读）→ 混链重开 → 全量重写后老页清零；掉电落在升级提交上 ⇒ 要么 v1 老状态要么 v2 新状态、本批原子
//      （第 13 轮的坑 103 重编号测试随重编号路径一起退役）
//   ③⑤ 层合并新规则：(c) 小组满 t 条就合（纯插入 run 数不再攒到 4t）；(d) 任意层最老连续前缀垃圾 ≥ 50% 就合（只合该合的前缀）；
//      (e) 顶层单条 ≥ 50% 脏重写一次、不反复重写；lvl_t=2 级联到顶层后 run 数仍有界
//   ④ mode 1 墙钟预算：预算用完 ⇒ maint_pending / deferred，后续提交补完；L0 积压时不受预算限制（run 数仍有界）；
//      mode 3 后台维护线程：并发读者 + 提交无错、数据一致，切回 mode 1 / 析构 / 重新 open 都 join 线程（TSan 版也跑）
// 判据：每段都 verify（含页所有权：预留页必须全部归还或入账）+ 与真值比对；失败打印第一处。单跑 ≈ 10 ~ 15 s（RamDev / build/ 下的文件）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>
#define private public
#include "fcs.h"
#undef private
#define G6_NO_MAIN
#include "g6.cpp"              // VolDev（易失页缓存 + 掉电 + 撕裂）
using namespace fcs;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Model { std::map<u64, u32> m; };     // 键 → tag（0 = 已删）

void putm(Store& s, Model& t, u64 k, u32 tag, u32 len = 200) {
    std::vector<u8> v(len, (u8)(tag * 7 + 1)); std::memcpy(v.data(), &tag, 4);
    s.put(k, v.data(), (u16)len); t.m[k] = tag;
}
int cmp_model(Store& s, const Model& t, const char* tag, u64 view = 0) {   // 第 16 轮：视图 64 位（u32 会把 ≥ 2^32 的快照版本截断）
    static u8 buf[65536]; u16 len = 0; int bad = 0;
    for (auto& kv : t.m) {
        RC rc = view ? s.get_at(kv.first, view, buf, 65535, &len) : s.get(kv.first, buf, 65535, &len);
        bool ok = kv.second == 0 ? rc == RC::NOTFOUND : (rc == RC::OK && len >= 4 && std::memcmp(buf, &kv.second, 4) == 0);
        if (!ok && bad++ < 3) std::printf("  %s FAIL: key %llu rc=%s len=%u want tag %u\n", tag, (unsigned long long)kv.first, rc_str(rc), len, kv.second);
    }
    return bad ? 1 : 0;
}
int vok(Store& s, const char* tag) {
    Report rep; RC rc = s.verify(&rep);
    if (rc != RC::OK || rep.errors) { std::printf("  %s FAIL: verify rc=%s errors=%u first=%s key=%llu\n", tag, rc_str(rc), rep.errors, rep.first_err, (unsigned long long)rep.first_key); return 1; }
    return 0;
}
u32 runs_at(Store& s, u32 lvl) { u32 n = 0; for (auto& kv : s.runs_) if (kv.second->lvl == lvl && kv.second->npages) ++n; return n; }
// 第 17 轮：家族数（同层不同 seq 的个数）。分片归并 / 按洞分片之后一个产物可能是几片（同 seq），"几条 run"要按家族数。
u32 fams_at(Store& s, u32 lvl) { std::set<u64> q; for (auto& kv : s.runs_) if (kv.second->lvl == lvl && kv.second->npages) q.insert(kv.second->seq); return (u32)q.size(); }

// ------------------------------------------------------------------ (1) ② 流式产物
// 3000 × 200 B 一批 ⇒ 每条 L0 run ≈ 155 页；lvl_t=4 ⇒ 4 条一组 ≈ 620 页 ≥ 2×STREAM_CHUNK ⇒ 走流式。
// cap=0 自动估；cap=40 故意估小（搬家路径）；cap=巨大 ⇒ 预留拿不到 ⇒ 退回老路径（stream_runs 必须为 0）。
int t_stream() {
    int bad = 0;
    struct Cfg { bool file, async; u32 cap; } cfgs[] = {
        {false, false, 0}, {false, true, 0}, {true, false, 0}, {true, true, 0},
        {false, false, 40}, {false, true, 40}, {true, true, 40}, {false, false, 100000000u},
    };
    u64 tot_runs = 0, tot_moves = 0, tot_pages = 0;
    for (const Cfg& cf : cfgs) {
        char tag[64]; std::snprintf(tag, sizeof tag, "t_stream(%s,%s,cap=%u)", cf.file ? "file" : "ram", cf.async ? "async" : "sync", cf.cap);
        const char* P = "build/g13_stream.db"; std::remove(P);
        std::unique_ptr<BlockDev> dev;
        if (cf.file) dev.reset(new FileDev(P, 8192)); else dev.reset(new RamDev(8192));
        static u8 arena[512 * PAGE];
        Model t, snap1, snap2; std::mt19937_64 rng(cf.cap + (cf.file ? 1 : 0) + (cf.async ? 2 : 0));
        const u32 N = 20000;
        u32 snap_v = 0;
        {
            Store s; if (s.open(dev.get(), arena, 512) != RC::OK) { std::printf("  %s open fail\n", tag); return 1; }
            s.set_lvl_t(4); s.set_maint_mode(1);
            s.dbg_stream_cap_ = cf.cap;
            if (cf.async) { s.set_io_threads(2); s.set_async_writes(true); }
            for (int c = 0; c < 30; ++c) {
                for (int i = 0; i < 3000; ++i) putm(s, t, 1 + rng() % N, 100 + c);
                if (RC r = s.commit(); r != RC::OK) { std::printf("  %s FAIL: commit %d rc=%s\n", tag, c, rc_str(r)); bad++; }
                bad += vok(s, tag);
                if (c % 3 == 2) bad += cmp_model(s, t, tag);
                if (c == 9) { if (s.snapshot() != RC::OK) { std::printf("  %s FAIL: snapshot\n", tag); bad++; } snap1 = t; snap_v = s.snap_version(0); }
                if (c == 13) bad += cmp_model(s, snap1, tag, snap_v);          // 快照视图：源被合并、产物流式落地后老视图仍完整
                if (c == 17) { if (s.forget_snapshots() != RC::OK) bad++; }
                if (c == 21) { if (s.snapshot() != RC::OK) bad++; snap2 = t; }
                if (c == 25) {                                                  // 回退到 21：流式产物页从未被老清单引用，回退后账目必须干净
                    if (RC r = s.rollback(0); r != RC::OK) { std::printf("  %s FAIL: rollback rc=%s\n", tag, rc_str(r)); bad++; }
                    t = snap2;
                    if (s.forget_snapshots() != RC::OK) bad++;
                    bad += vok(s, tag) + cmp_model(s, t, tag);
                }
            }
            auto ms = s.maint_stats();
            tot_runs += ms.stream_runs; tot_moves += ms.stream_moves; tot_pages += ms.stream_pages;
            if (cf.cap == 0 && ms.stream_runs == 0) { std::printf("  %s FAIL: 没有走过流式路径\n", tag); bad++; }
            if (cf.cap == 40 && (ms.stream_runs == 0 || ms.stream_moves == 0)) { std::printf("  %s FAIL: 估小却没有搬家（runs=%llu moves=%llu）\n", tag, (unsigned long long)ms.stream_runs, (unsigned long long)ms.stream_moves); bad++; }
            if (cf.cap >= 100000000u && ms.stream_runs != 0) { std::printf("  %s FAIL: 预留拿不到还走了流式\n", tag); bad++; }
            if (cf.async) s.set_async_writes(false);
            bad += vok(s, tag) + cmp_model(s, t, tag);
        }
        {
            static u8 arena2[512 * PAGE];
            Store s2; if (s2.open(dev.get(), arena2, 512) != RC::OK) { std::printf("  %s FAIL: reopen\n", tag); bad++; continue; }
            bad += vok(s2, tag) + cmp_model(s2, t, tag);
        }
        std::remove(P);
    }
    std::printf("  流式产物 8 组（sync/async × Ram/File × cap 自动/估小/拿不到）：流式 run %llu、落页 %llu、搬家 %llu  %s\n",
                (unsigned long long)tot_runs, (unsigned long long)tot_pages, (unsigned long long)tot_moves, bad ? "FAIL" : "ok");
    return bad;
}

// 小设备：预留拿不到 ⇒ 退一步 / 老路径；合并 NOSPACE ⇒ 尽力而为；提交 NOSPACE ⇒ 库仍可用、成功提交过的不丢
int t_stream_nospace() {
    int bad = 0;
    static u8 arena[256 * PAGE];
    RamDev dev(2600);                                   // 10 MB；活数据 ≈ 1.2 MB + 垃圾，L0 组 ≈ 620 页 + 预留 700 页很快装不下
    Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_stream_nospace open fail\n"); return 1; }
    s.set_lvl_t(4); s.set_maint_mode(1);
    Model t, committed; std::mt19937_64 rng(7);
    int nospace = 0, okc = 0;
    for (int c = 0; c < 40; ++c) {
        for (int i = 0; i < 3000; ++i) putm(s, t, 1 + rng() % 6000, 500 + c);
        RC rc = s.commit();
        if (rc == RC::OK) { committed = t; ++okc; }
        else if (rc == RC::NOSPACE) ++nospace;
        else { std::printf("  t_stream_nospace FAIL: commit rc=%s\n", rc_str(rc)); bad++; }
        bad += vok(s, "t_stream_nospace");
        if (bad) break;
    }
    auto ms = s.maint_stats();
    {
        static u8 arena2[256 * PAGE];
        Store s2; if (s2.open(&dev, arena2, 256) != RC::OK) { std::printf("  t_stream_nospace FAIL: reopen\n"); return bad + 1; }
        bad += vok(s2, "t_stream_nospace(reopen)");
        static u8 buf[65536]; u16 len = 0; int wrong = 0;
        for (auto& kv : committed.m) {
            RC rc = s2.get(kv.first, buf, 65535, &len);
            u32 tag = 0; if (rc == RC::OK && len >= 4) std::memcpy(&tag, buf, 4);
            if (!(rc == RC::OK && tag >= kv.second) && wrong++ < 3) std::printf("  t_stream_nospace FAIL: key %llu rc=%s tag %u want >= %u\n", (unsigned long long)kv.first, rc_str(rc), tag, kv.second);
        }
        if (wrong) bad++;
    }
    std::printf("  小设备（2600 页）流式回退：成功 %d / NOSPACE %d，流式 run %llu、搬家 %llu，重开 verify + 单调比对  %s\n",
                okc, nospace, (unsigned long long)ms.stream_runs, (unsigned long long)ms.stream_moves, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (2) ① 64 位序号 / 版本号 + v1 清单链（第 16 轮 P2 改写）
// 第 13 轮的 t_renumber / t_renumber_cut（坑 103 重编号）随 P2 退役：seq / ver 盘上 64 位，重编号这条路径已删除。
// 这里换成三段：
//   t_seq64      ：next_seq_ 预置到 SEQ_BASE + 2^33、vers_ 预置到 2^32 以上 ⇒ 提交 / 归并 / verify / 快照视图 / 重开全程不截断；
//   t_v1chain    ：当前代码写不出 v1，而三张 golden v1 镜像的清单全在 SB 里内联（nman=0）——清单链里 v1 页的重放没人盖到。
//                  用测试侧"降级器"把一张刚写出的 v2 镜像（有 ≥ 3 页清单链）原地改成 v1 布局（SB 40 B 头 + 40 B 项、PK_MAN 页），
//                  再：只读挂载（一个字节不写回、mounted_format()==1）→ 写 + 提交 ⇒ 第一次写 SB 就是 v2（format_version()==2）
//                  → 继续提交让链里 v1 页 / v2 页混着 → 重开 ⇒ mounted_format()==2、verify、与真值比对；
//   t_v1chain_cut：掉电落在升级那次提交的任意一笔写上 ⇒ 重开要么还是 v1 老状态、要么是 v2 新状态，本批原子，之后能继续提交。
u64 vers_at(Store& s) { return s.last_commit(); }

int t_seq64() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(16384);
    Model t; std::mt19937_64 rng(13);
    const u32 N = 5000;
    const u64 SEQ0 = SEQ_BASE + (1ull << 33) + 7, VER0 = (1ull << 32) + 1000;
    u64 next_before = 0, v_close = 0;
    {
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_seq64 open fail\n"); return 1; }
        s.set_maint_mode(1);
        s.next_seq_ = SEQ0; s.vers_ = VER0;                 // 跨过"低 32 位"边界：任何 u32 截断都会在这里现形
        auto batch = [&](u32 tag, int n = 1000) { for (int i = 0; i < n; ++i) putm(s, t, 1 + rng() % N, tag, 120); };
        for (int c = 0; c < 24; ++c) { batch(1000 + c); if (s.commit() != RC::OK) { std::printf("  t_seq64 FAIL: commit %d\n", c); return 1; } }
        bad += vok(s, "t_seq64") + cmp_model(s, t, "t_seq64");
        if (vers_at(s) != VER0 + 24) { std::printf("  t_seq64 FAIL: last_commit=%llu（应 %llu）\n", (unsigned long long)vers_at(s), (unsigned long long)(VER0 + 24)); bad++; }
        // 快照视图跨 2^32：抓一个版本，再覆盖写，按旧版本读回旧值
        Model held = t; u64 hv = 0;
        if (s.snapshot() != RC::OK) { std::printf("  t_seq64 FAIL: snapshot\n"); bad++; }
        hv = vers_at(s);
        batch(2000, 2000); if (s.commit() != RC::OK) { std::printf("  t_seq64 FAIL: commit after snap\n"); bad++; }
        bad += cmp_model(s, held, "t_seq64(snap)", hv) + cmp_model(s, t, "t_seq64(cur)");
        s.forget_snapshots();
        batch(2001, 2000); if (s.commit() != RC::OK) { std::printf("  t_seq64 FAIL: commit after forget\n"); bad++; }
        bad += vok(s, "t_seq64(b)") + cmp_model(s, t, "t_seq64(b)");
        for (auto& kv : s.runs_) if (kv.second->npages && kv.second->seq < SEQ0) { std::printf("  t_seq64 FAIL: run seq=%llx < 预置值\n", (unsigned long long)kv.second->seq); bad++; break; }
        next_before = s.next_seq_; v_close = vers_at(s);
        if (next_before <= SEQ0 + 24) { std::printf("  t_seq64 FAIL: next_seq_ 没有前移（%llx）\n", (unsigned long long)next_before); bad++; }
    }
    {   // 重开：seq / ver 一个比特不少
        static u8 arena2[512 * PAGE];
        Store s2; if (s2.open(&dev, arena2, 512) != RC::OK) { std::printf("  t_seq64 FAIL: reopen\n"); return bad + 1; }
        // 关前最后一次提交之后 mode 1 的维护可能已在内存里合并（增量留在下一提交点落盘），所以 run 集合允许不同；
        // 但盘上每条 run 的 seq 必须 ≥ 预置值（高 32 位没丢）、next_seq_ 介于 (SEQ0+24, 关前值]、版本号与关前一致。
        u32 nlow = 0; for (auto& kv : s2.runs_) if (kv.second->npages && kv.second->seq < SEQ0) ++nlow;
        if (nlow) { std::printf("  t_seq64 FAIL: 重开后 %u 条 run 的 seq 低于预置值（高 32 位被截断）\n", nlow); bad++; }
        if (s2.next_seq_ > next_before || s2.next_seq_ <= SEQ0 + 24) { std::printf("  t_seq64 FAIL: 重开 next_seq_=%llx（关前 %llx）\n", (unsigned long long)s2.next_seq_, (unsigned long long)next_before); bad++; }
        if (vers_at(s2) != v_close) { std::printf("  t_seq64 FAIL: 重开 last_commit=%llu（关前 %llu）\n", (unsigned long long)vers_at(s2), (unsigned long long)v_close); bad++; }
        if (s2.mounted_format() != 2 || s2.format_version() != 2) { std::printf("  t_seq64 FAIL: fmt mounted=%u cur=%u\n", s2.mounted_format(), s2.format_version()); bad++; }
        bad += vok(s2, "t_seq64(reopen)") + cmp_model(s2, t, "t_seq64(reopen)");
        for (int c = 0; c < 4; ++c) { for (int i = 0; i < 500; ++i) putm(s2, t, 1 + rng() % N, 3000 + c, 120); if (s2.commit() != RC::OK) { std::printf("  t_seq64 FAIL: reopen commit %d\n", c); bad++; } }
        bad += vok(s2, "t_seq64(reopen+)") + cmp_model(s2, t, "t_seq64(reopen+)");
    }
    std::printf("  64 位序号 / 版本号：next_seq_=SEQ_BASE+2^33、vers_=2^32+1000 起 31 次提交 + 快照视图 + 重开  %s\n", bad ? "FAIL" : "ok");
    return bad;
}

// 测试侧"降级器"：把一张 v2 镜像原地改成 v1 盘上布局。只在 ver/seq < 2^32、flags==0、SB 内联 ≤ 32 条时无损，否则返回 false。
// v1 SB 40 B：magic('FCSB') vers32 state chk tail man_first man_last nman n_inl pad；v1 项 40 B：op lvl pad2 ver32 key last page0 npages nbytes seq32。
void enc_v1(u8* e, const Store::ManEnt& m) {
    std::memset(e, 0, MENT_V1); e[0] = m.op; e[1] = m.lvl;
    u32 v = (u32)m.ver, q = (u32)m.seq;
    std::memcpy(e + 4, &v, 4); std::memcpy(e + 8, &m.key, 8); std::memcpy(e + 16, &m.last, 8);
    std::memcpy(e + 24, &m.page0, 4); std::memcpy(e + 28, &m.npages, 4); std::memcpy(e + 32, &m.nbytes, 4); std::memcpy(e + 36, &q, 4);
}
bool lossless_v1(const Store::ManEnt& m) { return !(m.ver >> 32) && !(m.seq >> 32) && m.flags == 0; }
bool downgrade_to_v1(RamDev& dev, u32 scan_pages, u32* nsb_out = nullptr, u32* npg_out = nullptr) {
    u32 nsb = 0, npg = 0;
    // 先找最新的有效槽：它必须能无损降级；更老的那个槽装不下（内联 > 32 条）就擦掉（挂载只会用最新槽，另一槽只是撕裂时的退路）
    int newest = -1; u64 newest_v = 0;
    for (u32 slot = 0; slot < 2; ++slot) {
        const u8* p = dev.raw(slot);
        if (rd32(p) != PK_SB2 || rd32(p + 8) != COMMIT_WORD || rd32(p + 32) > INLINE_MAX) continue;
        if (Store::sb_chk(p, SB_HDR, rd32(p + 32), MENT) != rd32(p + 12)) continue;
        u64 v = (u64)rd32(p + 36) | ((u64)rd32(p + 40) << 32);
        if (newest < 0 || v >= newest_v) { newest = (int)slot; newest_v = v; }
    }
    if (newest < 0) return false;
    for (u32 slot = 0; slot < 2; ++slot) {
        u8* p = dev.raw(slot);
        if (rd32(p) != PK_SB2 || rd32(p + 8) != COMMIT_WORD) continue;
        u32 n_inl = rd32(p + 32), vers_lo = rd32(p + 36), vers_hi = rd32(p + 40);
        if (n_inl > INLINE_MAX) continue;
        if (Store::sb_chk(p, SB_HDR, n_inl, MENT) != rd32(p + 12)) continue;       // 撕裂的槽不管
        if (vers_hi) return false;
        if (n_inl > INLINE_MAX_V1) { if ((int)slot == newest) return false; std::memset(p, 0xFF, PAGE); continue; }
        std::vector<Store::ManEnt> ents;
        for (u32 i = 0; i < n_inl; ++i) { ents.push_back(Store::decode_man(p + SB_HDR + (size_t)i * MENT)); if (!lossless_v1(ents.back())) return false; }
        std::vector<u8> out(SB_HDR_V1 + (size_t)n_inl * MENT_V1, 0);
        wr32(out.data(), PK_SB); wr32(out.data() + 4, vers_lo); wr32(out.data() + 8, COMMIT_WORD);
        std::memcpy(out.data() + 16, p + 16, 16);                                   // tail man_first man_last nman 同位
        wr32(out.data() + 32, n_inl);
        for (u32 i = 0; i < n_inl; ++i) enc_v1(out.data() + SB_HDR_V1 + (size_t)i * MENT_V1, ents[i]);
        wr32(out.data() + 12, Store::sb_chk(out.data(), SB_HDR_V1, n_inl, MENT_V1));
        std::memset(p, 0xFF, PAGE); std::memcpy(p, out.data(), out.size()); ++nsb;
    }
    for (u32 pid = 2; pid < scan_pages; ++pid) {
        u8* p = dev.raw(pid);
        if (rd32(p) != PK_MAN2) continue;
        u32 n = rd32(p + 8);
        if (n > MAN_PER_PG || crc32c(p + MHDR, n * MENT) != rd32(p + 12)) return false;
        std::vector<Store::ManEnt> ents;
        for (u32 i = 0; i < n; ++i) { ents.push_back(Store::decode_man(p + MHDR + (size_t)i * MENT)); if (!lossless_v1(ents.back())) return false; }
        std::vector<u8> body((size_t)n * MENT_V1, 0);
        for (u32 i = 0; i < n; ++i) enc_v1(body.data() + (size_t)i * MENT_V1, ents[i]);
        wr32(p, PK_MAN); std::memcpy(p + MHDR, body.data(), body.size()); wr32(p + 12, crc32c(body.data(), (u32)body.size()));
        ++npg;
    }
    if (nsb_out) *nsb_out = nsb;
    if (npg_out) *npg_out = npg;
    return nsb > 0;
}
u32 count_man_pages(RamDev& dev, u32 scan_pages, u32 magic) { u32 n = 0; for (u32 pid = 2; pid < scan_pages; ++pid) if (rd32(dev.raw(pid)) == magic) ++n; return n; }

// 在 dev 上造一座"老库"：seq 从 1000 起（v1 能装下）、≥ 3 页清单链、SB 内联 ≤ 32 条；返回 tail（扫描范围）。0 = 失败。
u32 build_then_downgrade(RamDev& dev, Model& t, std::mt19937_64& rng, const char* tag, u32* runs_out, u32* nman_out, u32* npg_out) {
    static u8 arena[512 * PAGE];
    u32 tail = 0;
    {
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  %s open fail\n", tag); return 0; }
        s.next_seq_ = 1000;
        int c = 0;
        for (; c < 3000; ++c) {
            for (int i = 0; i < 25; ++i) putm(s, t, 1 + rng() % 3000, 100 + (u32)c, 100);
            if (s.commit() != RC::OK) { std::printf("  %s FAIL: commit %d\n", tag, c); return 0; }
            if (c >= 200 && s.nman_ >= 3 && s.pend_.size() >= 1 && s.pend_.size() <= INLINE_MAX_V1) break;
        }
        if (c >= 3000) { std::printf("  %s FAIL: 3000 次提交还没凑出想要的清单链形状（nman=%u pend=%zu）\n", tag, s.nman_, s.pend_.size()); return 0; }
        if (vok(s, tag) || cmp_model(s, t, tag)) return 0;
        tail = s.tail_; *runs_out = s.run_count(); *nman_out = s.nman_;
    }
    u32 nsb = 0, npg = 0;
    if (!downgrade_to_v1(dev, tail, &nsb, &npg)) { std::printf("  %s FAIL: 降级失败（nsb=%u npg=%u）\n", tag, nsb, npg); return 0; }
    if (npg < 3 || count_man_pages(dev, tail, PK_MAN2) != 0) { std::printf("  %s FAIL: 降级后仍有 v2 清单页（npg=%u）\n", tag, npg); return 0; }
    *npg_out = npg;
    return tail;
}

int t_v1chain() {
    int bad = 0;
    RamDev dev(16384);
    Model t; std::mt19937_64 rng(16);
    u32 runs0 = 0, nman0 = 0, npg0 = 0;
    u32 tail = build_then_downgrade(dev, t, rng, "t_v1chain", &runs0, &nman0, &npg0);
    if (!tail) return 1;
    std::vector<u8> before(dev.raw(0), dev.raw(0) + (size_t)tail * PAGE);
    static u8 arena[512 * PAGE];
    {   // 只读：识别为 v1、一个字节不写回
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_v1chain FAIL: v1 mount\n"); return bad + 1; }
        if (s.mounted_format() != 1 || s.format_version() != 1) { std::printf("  t_v1chain FAIL: 应识别为 v1（mounted=%u fmt=%u）\n", s.mounted_format(), s.format_version()); bad++; }
        if (s.run_count() != runs0 || s.nman_ != nman0) { std::printf("  t_v1chain FAIL: v1 重放 runs=%u nman=%u（降级前 %u / %u）\n", s.run_count(), s.nman_, runs0, nman0); bad++; }
        bad += vok(s, "t_v1chain(ro)") + cmp_model(s, t, "t_v1chain(ro)");
        if (std::memcmp(before.data(), dev.raw(0), before.size()) != 0) { std::printf("  t_v1chain FAIL: 只读挂载改了镜像\n"); bad++; }
        // 升级：第一次写 SB 就是 v2；链里老页不动
        for (int i = 0; i < 25; ++i) putm(s, t, 1 + rng() % 3000, 5000, 100);
        if (s.commit() != RC::OK) { std::printf("  t_v1chain FAIL: 升级提交\n"); bad++; }
        if (s.format_version() != 2 || s.mounted_format() != 1) { std::printf("  t_v1chain FAIL: 升级后 fmt=%u mounted=%u\n", s.format_version(), s.mounted_format()); bad++; }
        u32 v1pg = count_man_pages(dev, s.tail_, PK_MAN), v2pg = count_man_pages(dev, s.tail_, PK_MAN2);
        if (v1pg == 0) { std::printf("  t_v1chain FAIL: 升级提交把老清单页重写了（v1 页 0）\n"); bad++; }
        bad += vok(s, "t_v1chain(up)") + cmp_model(s, t, "t_v1chain(up)");
        // 继续提交到链里出现 v2 页（混链），但别触发全量清单重写（MAN_SNAP_AT）
        int c = 0; for (; c < 400 && count_man_pages(dev, s.tail_, PK_MAN2) == 0; ++c) { for (int i = 0; i < 25; ++i) putm(s, t, 1 + rng() % 3000, 6000 + (u32)c, 100); if (s.commit() != RC::OK) { std::printf("  t_v1chain FAIL: 混链提交 %d\n", c); bad++; break; } }
        v1pg = count_man_pages(dev, s.tail_, PK_MAN); v2pg = count_man_pages(dev, s.tail_, PK_MAN2);
        if (!v1pg || !v2pg) { std::printf("  t_v1chain FAIL: 没造出混链（v1 页 %u / v2 页 %u）\n", v1pg, v2pg); bad++; }
        bad += vok(s, "t_v1chain(mix)") + cmp_model(s, t, "t_v1chain(mix)");
        std::printf("  v1 清单链：降级 %u 页清单 + %u 条 run → 只读挂载 → 升级提交 → 混链（v1 页 %u / v2 页 %u，%d 次提交）  %s\n", npg0, runs0, v1pg, v2pg, c, bad ? "FAIL" : "ok");
    }
    {   // 重开：SB v2、链里新老页混着重放
        static u8 arena2[512 * PAGE];
        Store s; if (s.open(&dev, arena2, 512) != RC::OK) { std::printf("  t_v1chain FAIL: 混链重开\n"); return bad + 1; }
        if (s.mounted_format() != 2 || s.format_version() != 2) { std::printf("  t_v1chain FAIL: 重开 mounted=%u fmt=%u\n", s.mounted_format(), s.format_version()); bad++; }
        bad += vok(s, "t_v1chain(reopen)") + cmp_model(s, t, "t_v1chain(reopen)");
        // 再把链推过 MAN_SNAP_AT：全量清单重写之后老页应全部释放（整库 v2）
        int c = 0; for (; c < 6000 && count_man_pages(dev, s.tail_, PK_MAN) != 0; ++c) { for (int i = 0; i < 25; ++i) putm(s, t, 1 + rng() % 3000, 7000 + (u32)(c % 1000), 100); if (s.commit() != RC::OK) { std::printf("  t_v1chain FAIL: 重写期提交 %d\n", c); bad++; break; } }
        if (count_man_pages(dev, s.tail_, PK_MAN) != 0) { std::printf("  t_v1chain FAIL: %d 次提交后老清单页还在\n", c); bad++; }
        bad += vok(s, "t_v1chain(rewrite)") + cmp_model(s, t, "t_v1chain(rewrite)");
        std::printf("  混链重开 → 再 %d 次提交触发全量清单重写 ⇒ 老页清零  %s\n", c, bad ? "FAIL" : "ok");
    }
    return bad;
}

// 掉电落在升级那次提交上：重开后要么 v1 老状态、要么 v2 新状态；本批原子；之后继续提交正常
int t_v1chain_cut() {
    int bad = 0;
    VolDev dev(16384);
    Model t; std::mt19937_64 rng(21);
    u32 runs0 = 0, nman0 = 0, npg0 = 0;
    dev.cut_at = ~0ull;
    u32 tail = build_then_downgrade(dev, t, rng, "t_v1chain_cut", &runs0, &nman0, &npg0);
    if (!tail) return 1;
    dev.do_sync();
    std::memcpy(dev.stable_.data(), dev.raw(0), dev.stable_.size());      // 降级器直接改的是页缓存：介质也同步成 v1
    std::vector<u8> v1img(dev.raw(0), dev.raw(0) + (size_t)tail * PAGE);
    static u8 arena[512 * PAGE];
    int old_n = 0, new_n = 0, atomic_bad = 0;
    const u64 cuts[] = {1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 22, 30, 45, 70};
    for (u64 cut : cuts) {
        // 每个切点都从同一张 v1 镜像出发
        dev.reboot(1ull << 40);
        std::memcpy(dev.stable_.data(), v1img.data(), v1img.size()); dev.reboot(1ull << 40);
        dev.writes = 0; dev.cut_at = cut;
        Model before = t, after = t;
        {
            Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_v1chain_cut FAIL: v1 mount (cut=%llu)\n", (unsigned long long)cut); bad++; continue; }
            if (s.mounted_format() != 1) { std::printf("  t_v1chain_cut FAIL: cut=%llu 起点不是 v1\n", (unsigned long long)cut); bad++; }
            for (int i = 0; i < 600; ++i) putm(s, after, 1 + rng() % 3000, 900 + (u32)cut, 100);
            (void)s.commit();                                                 // 设备半路死掉：返回值不作数
        }
        dev.reboot(1ull << 40); dev.cut_at = ~0ull;
        Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_v1chain_cut FAIL: remount after cut=%llu\n", (unsigned long long)cut); bad++; continue; }
        bad += vok(s, "t_v1chain_cut");
        int saw_new = 0, saw_old = 0; static u8 buf[4096]; u16 len = 0;
        for (auto& kv : after.m) {
            if (kv.second != 900 + (u32)cut) continue;
            RC rc = s.get(kv.first, buf, sizeof buf, &len); u32 tag = 0; if (rc == RC::OK && len >= 4) std::memcpy(&tag, buf, 4);
            if (rc == RC::OK && tag == kv.second) ++saw_new;
            else {
                auto it = before.m.find(kv.first);
                bool ok_old = it == before.m.end() ? rc == RC::NOTFOUND : (rc == RC::OK && tag == it->second);
                if (ok_old) ++saw_old; else { if (atomic_bad++ < 3) std::printf("  t_v1chain_cut FAIL: cut=%llu key %llu rc=%s tag %u（既非新也非老）\n", (unsigned long long)cut, (unsigned long long)kv.first, rc_str(rc), tag); }
            }
        }
        if (saw_new && saw_old) { std::printf("  t_v1chain_cut FAIL: cut=%llu 本批不原子（新 %d / 老 %d）\n", (unsigned long long)cut, saw_new, saw_old); bad++; }
        Model cur = saw_new ? after : before;
        if (saw_new) { if (s.mounted_format() != 2) { std::printf("  t_v1chain_cut FAIL: cut=%llu 新批落地却不是 v2（mounted=%u）\n", (unsigned long long)cut, s.mounted_format()); bad++; } ++new_n; }
        else { if (s.mounted_format() != 1) { std::printf("  t_v1chain_cut FAIL: cut=%llu 回到老状态却不是 v1（mounted=%u）\n", (unsigned long long)cut, s.mounted_format()); bad++; } ++old_n; }
        bad += cmp_model(s, cur, "t_v1chain_cut");
        for (int i = 0; i < 300; ++i) putm(s, cur, 1 + rng() % 3000, 950 + (u32)cut, 100);   // 掉电之后继续用（这次一定升到 v2）
        if (s.commit() != RC::OK) { std::printf("  t_v1chain_cut FAIL: cut=%llu 之后提交失败\n", (unsigned long long)cut); bad++; }
        if (s.format_version() != 2) { std::printf("  t_v1chain_cut FAIL: cut=%llu 之后提交仍是 v%u\n", (unsigned long long)cut, s.format_version()); bad++; }
        bad += vok(s, "t_v1chain_cut(after)") + cmp_model(s, cur, "t_v1chain_cut(after)");
        dev.do_sync();
    }
    if (atomic_bad) bad++;
    std::printf("  v1 升级 × 掉电 %zu 个切点：重开后仍 v1 %d / 已 v2 %d，本批原子、之后可继续提交  %s\n", sizeof cuts / sizeof cuts[0], old_n, new_n, bad ? "FAIL" : "ok");
    return bad;
}

// ------------------------------------------------------------------ (3) ③⑤ 层合并规则
int t_merge_rules() {
    int bad = 0;
    // (c) 纯插入小 run：cheap 开 ⇒ run 数峰值 ≤ 2t+2；cheap 关 ⇒ 攒到 4t
    u32 peak_on = 0, peak_off = 0;
    for (int on = 1; on >= 0; --on) {
        static u8 arena[256 * PAGE];
        RamDev dev(8192); Model t;
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_merge(c) open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_merge_cheap_pages(on ? 1024 : 0);
        u32 peak = 0; u64 k = 1;
        for (int c = 0; c < 72; ++c) {
            for (int i = 0; i < 500; ++i) putm(s, t, k++, 10 + c, 32);
            if (s.commit() != RC::OK) { std::printf("  t_merge(c) FAIL: commit\n"); bad++; }
            peak = std::max(peak, s.run_count());
            if (c % 24 == 23) bad += vok(s, "t_merge(c)") + cmp_model(s, t, "t_merge(c)");
        }
        (on ? peak_on : peak_off) = peak;
        const u32 t_ = s.lvl_t();
        if (on && peak > 2 * t_ + 2) { std::printf("  t_merge(c) FAIL: cheap 开着 run 数峰值 %u > %u\n", peak, 2 * t_ + 2); bad++; }
        if (on && s.run_count() > 4) { std::printf("  t_merge(c) FAIL: 72 次纯插入后还有 %u 条 run\n", s.run_count()); bad++; }
    }
    if (peak_off <= peak_on) { std::printf("  t_merge(c) FAIL: 关掉 cheap 峰值 %u 应大于开着的 %u\n", peak_off, peak_on); bad++; }
    std::printf("  (c) 纯插入 72 批 × 500 × 32 B：run 数峰值 cheap=1024 → %u，cheap=0 → %u  %s\n", peak_on, peak_off, bad ? "FAIL" : "ok");
    // (d) 不满 t 条的层：A、B 各 60% 死（没有一条全死 ⇒ 零写退休帮不上忙）+ C 全活 ⇒ 只合 A+B（重写 160 页换回 240 页），C 原样
    {
        static u8 arena[256 * PAGE];
        RamDev dev(8192); Model t;
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_merge(d) open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_merge_cheap_pages(0);
        auto drain = [&] { for (int i = 0; i < 200 && s.maintain(8) == RC::MORE; ++i) {} };
        auto fill = [&](u64 lo, u64 hi, u32 tag) { for (u64 k = lo; k < hi; ++k) putm(s, t, k, tag); if (s.commit() != RC::OK) { std::printf("  t_merge(d) FAIL: commit\n"); bad++; } drain(); };
        fill(1, 4001, 1); fill(4001, 8001, 2);              // A、B（各 ≈ 200 页）
        const u32 n2 = s.run_count();
        for (u64 k = 1; k <= 2400; ++k) putm(s, t, k, 3);   // C：盖掉 A 的 60% + B 的 60%
        for (u64 k = 4001; k <= 6400; ++k) putm(s, t, k, 3);
        if (s.commit() != RC::OK) bad++;
        drain();
        const u32 n3 = s.run_count();
        u64 dead_bytes = 0, tot_bytes = 0;
        for (auto& kv : s.runs_) { tot_bytes += kv.second->nbytes; dead_bytes += kv.second->nbytes - std::min<u64>(kv.second->live_bytes, kv.second->nbytes); }
        if (n2 != 2) { std::printf("  t_merge(d) FAIL: A,B 之后 %u 条 run\n", n2); bad++; }
        if (n3 != 2) { std::printf("  t_merge(d) FAIL: C 之后 %u 条 run（应为 A+B 产物 + C = 2）\n", n3); bad++; }
        if (tot_bytes && dead_bytes * 10 > tot_bytes) { std::printf("  t_merge(d) FAIL: 剩余 run 垃圾 %llu / %llu\n", (unsigned long long)dead_bytes, (unsigned long long)tot_bytes); bad++; }
        bad += vok(s, "t_merge(d)") + cmp_model(s, t, "t_merge(d)");
        std::printf("  (d) 不满 t 条的脏前缀：A,B → %u 条，+C（A、B 各 60%% 死）→ %u 条，剩余垃圾 %.0f%%  %s\n", n2, n3, tot_bytes ? 100.0 * dead_bytes / tot_bytes : 0.0, bad ? "FAIL" : "ok");
    }
    // (e) 顶层单条 ≥ 50% 脏 ⇒ 重写一次（页数按活数据缩），之后不再反复重写
    {
        static u8 arena[256 * PAGE];
        RamDev dev(8192); Model t;
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_merge(e) open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_merge_cheap_pages(0);
        auto drain = [&] { for (int i = 0; i < 200 && s.maintain(8) == RC::MORE; ++i) {} };
        for (u64 k = 1; k <= 8000; ++k) putm(s, t, k, 1);
        if (s.commit() != RC::OK) bad++;
        drain();
        if (s.run_count() != 1) { std::printf("  t_merge(e) FAIL: 建库后 %u 条 run\n", s.run_count()); bad++; }
        RunPtr top = s.runs_.begin()->second; top->lvl = Store::LVL_MAX;   // 层号只是分组提示：直接把它当顶层 run
        const u32 pg0 = top->npages;
        for (u64 k = 1; k <= 6000; ++k) putm(s, t, k, 2);     // 一批盖掉 75%
        if (s.commit() != RC::OK) bad++;
        drain();                                              // 提交后的维护受 20 ms 预算限制：这里把欠的活干完
        RunPtr ntop = nullptr; for (auto& kv : s.runs_) if (kv.second->lvl == Store::LVL_MAX) ntop = kv.second;
        const u32 pg1 = ntop ? ntop->npages : 0, rid1 = ntop ? ntop->page0 : 0;
        if (!ntop) { std::printf("  t_merge(e) FAIL: 顶层 run 不见了\n"); bad++; }
        else if (pg1 * 10 > pg0 * 4) { std::printf("  t_merge(e) FAIL: 顶层 run 没有重写（%u → %u 页）\n", pg0, pg1); bad++; }
        for (u64 k = 20001; k <= 20300; ++k) putm(s, t, k, 5);   // 不相干的小批 + 再维护：干净的顶层 run 不该再被碰
        if (s.commit() != RC::OK) bad++;
        drain();
        RunPtr ntop2 = nullptr; for (auto& kv : s.runs_) if (kv.second->lvl == Store::LVL_MAX) ntop2 = kv.second;
        if (!ntop2 || ntop2->page0 != rid1) { std::printf("  t_merge(e) FAIL: 干净的顶层 run 又被重写了\n"); bad++; }
        bad += vok(s, "t_merge(e)") + cmp_model(s, t, "t_merge(e)");
        std::printf("  (e) 顶层单条 75%% 脏：%u → %u 页，之后维护不再碰它  %s\n", pg0, pg1, bad ? "FAIL" : "ok");
    }
    // (e2) 非顶层的单条大 run（lvl 1，≥ 256 页）被范围删除盖掉 60% ⇒ 单条重写（走分片作业），产物升到 lvl 2、只剩活数据
    {
        static u8 arena[256 * PAGE];
        RamDev dev(8192); Model t;
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_merge(e2) open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_merge_cheap_pages(0); s.set_merge_budget_pages(64);
        auto drain = [&] { for (int i = 0; i < 400 && s.maintain(8) == RC::MORE; ++i) {} };
        for (u64 k = 1; k <= 16000; ++k) putm(s, t, k, 1);
        if (s.commit() != RC::OK) bad++;
        drain();
        RunPtr r1 = s.runs_.begin()->second; r1->lvl = 1;
        const u32 pg0 = r1->npages, rid0 = r1->page0;
        s.del_range(1, 9601); for (auto it = t.m.begin(); it != t.m.end(); ++it) if (it->first <= 9600) it->second = 0;
        if (s.commit() != RC::OK) bad++;
        drain();
        const u64 slices0 = s.maint_stats().slices;
        RunPtr nr = nullptr; for (auto& kv : s.runs_) if (kv.second->npages >= 32) nr = kv.second;   // 范围删除本身不产生 run；只剩重写产物
        if (!nr || nr->page0 == rid0) { std::printf("  t_merge(e2) FAIL: lvl 1 单条 60%% 被盖住没有重写\n"); bad++; }
        else {
            if (nr->lvl != 2) { std::printf("  t_merge(e2) FAIL: 产物层号 %u（应为 2）\n", nr->lvl); bad++; }
            if (nr->npages * 10 > pg0 * 5) { std::printf("  t_merge(e2) FAIL: 产物 %u 页（源 %u 页，应 ≈ 40%%）\n", nr->npages, pg0); bad++; }
        }
        if (slices0 == 0) { std::printf("  t_merge(e2) FAIL: 单条重写没有走分片作业\n"); bad++; }
        bad += vok(s, "t_merge(e2)") + cmp_model(s, t, "t_merge(e2)");
        std::printf("  (e2) lvl 1 单条 %u 页被范围删除盖掉 60%%：→ %u 页 lvl %u（分片 %llu）  %s\n", pg0, nr ? nr->npages : 0, nr ? nr->lvl : 0, (unsigned long long)slices0, bad ? "FAIL" : "ok");
    }
    // (f) lvl_t=2 级联到顶层：顶层也参与合并，纯插入 60 批后 run 数有界
    {
        static u8 arena[256 * PAGE];
        RamDev dev(16384); Model t;
        Store s; if (s.open(&dev, arena, 256) != RC::OK) { std::printf("  t_merge(f) open fail\n"); return 1; }
        s.set_maint_mode(1); s.set_lvl_t(2);
        u32 peak = 0; u64 k = 1;
        for (int c = 0; c < 60; ++c) {
            for (int i = 0; i < 400; ++i) putm(s, t, k++, 20 + c, 64);
            if (s.commit() != RC::OK) bad++;
            peak = std::max(peak, s.run_count());
        }
        const u32 top = runs_at(s, Store::LVL_MAX), topf = fams_at(s, Store::LVL_MAX);
        // 第 17 轮 ②(c3)：顶层产物（≈430 页 < 1024）装不进任何洞时按洞切成 2~4 片（同 seq 一个家族）⇒ 顶层按**家族**数判
        if (peak > 12) { std::printf("  t_merge(f) FAIL: lvl_t=2 级联 run 数峰值 %u\n", peak); bad++; }
        if (topf > 2) { std::printf("  t_merge(f) FAIL: 顶层 %u 个家族（%u 条 run）没有合并\n", topf, top); bad++; }
        if (top > 4 * 2) { std::printf("  t_merge(f) FAIL: 顶层 %u 条 run（家族 %u）：片数超过按洞分片的上限\n", top, topf); bad++; }
        bad += vok(s, "t_merge(f)") + cmp_model(s, t, "t_merge(f)");
        std::printf("  (f) lvl_t=2 纯插入 60 批：run 数峰值 %u、顶层 %u 个家族 / %u 条 run、最终 %u 条  %s\n", peak, topf, top, s.run_count(), bad ? "FAIL" : "ok");
    }
    return bad;
}

// ------------------------------------------------------------------ (4) ④ 预算 + mode 3
int t_budget() {
    int bad = 0;
    static u8 arena[512 * PAGE];
    RamDev dev(32768); Model t; std::mt19937_64 rng(31);
    Store s; if (s.open(&dev, arena, 512) != RC::OK) { std::printf("  t_budget open fail\n"); return 1; }
    s.set_maint_mode(1); s.set_maint_budget_us(1);          // 1 µs：第一个单元之后必然超预算
    const u32 N = 20000;
    u32 pending_seen = 0, peak = 0;
    for (int c = 0; c < 48; ++c) {
        for (int i = 0; i < 1200; ++i) putm(s, t, 1 + rng() % N, 100 + c, 400);
        if (s.commit() != RC::OK) { std::printf("  t_budget FAIL: commit %d\n", c); bad++; }
        if (s.maint_pending()) ++pending_seen;
        peak = std::max(peak, s.run_count());
        if (c % 12 == 11) bad += vok(s, "t_budget") + cmp_model(s, t, "t_budget");
    }
    auto ms = s.maint_stats();
    const u32 bound = (u32)s.lvl_hist().size() * s.lvl_t() * 4 + 4;
    if (ms.deferred == 0 || pending_seen == 0) { std::printf("  t_budget FAIL: 预算 1 µs 却从未推迟（deferred=%llu pending=%u）\n", (unsigned long long)ms.deferred, pending_seen); bad++; }
    if (peak > bound) { std::printf("  t_budget FAIL: run 数峰值 %u 超过兜底线 %u（积压时应不受预算限制）\n", peak, bound); bad++; }
    s.set_maint_budget_us(0);                               // 不限：下一次提交把欠的活干完
    for (int i = 0; i < 200; ++i) putm(s, t, 1 + rng() % N, 999, 400);
    if (s.commit() != RC::OK) bad++;
    if (s.maint_pending()) { std::printf("  t_budget FAIL: 预算不限仍有欠账\n"); bad++; }
    bad += vok(s, "t_budget") + cmp_model(s, t, "t_budget");
    std::printf("  mode 1 预算 1 µs × 48 次提交：推迟 %llu 次、提交后有欠账 %u 次、run 峰值 %u（兜底 %u）；预算放开后欠账清零  %s\n",
                (unsigned long long)ms.deferred, pending_seen, peak, bound, bad ? "FAIL" : "ok");
    return bad;
}

int t_mode3() {
    int bad = 0;
    static u8 arena[1024 * PAGE];
    RamDev dev(65536); Model t; std::mt19937_64 rng(41);
    const u32 N = 20000;
    std::mutex tm;                                           // 模型只在主线程改；读者只读引擎
    {
        Store s; if (s.open(&dev, arena, 1024) != RC::OK) { std::printf("  t_mode3 open fail\n"); return 1; }
        s.set_shared_reads(true); s.set_maint_mode(3); s.set_merge_budget_pages(16);
        if (s.maint_mode() != 3 || !s.mthr_.joinable()) { std::printf("  t_mode3 FAIL: 线程没起来\n"); bad++; }
        std::atomic<bool> stop{false}; std::atomic<u64> reads{0}, errs{0};
        std::thread rd([&] {
            std::mt19937 r(9); u8 buf[4096]; u16 len;
            while (!stop) for (int i = 0; i < 64; ++i) { RC rc = s.get(1 + r() % N, buf, sizeof buf, &len); if (rc != RC::OK && rc != RC::NOTFOUND) ++errs; ++reads; }
        });
        u32 peak = 0;
        for (int c = 0; c < 60; ++c) {
            for (int i = 0; i < 1500; ++i) putm(s, t, 1 + rng() % N, 100 + c, 120);
            if (s.commit() != RC::OK) { std::printf("  t_mode3 FAIL: commit %d\n", c); bad++; }
            peak = std::max(peak, s.run_count());
            if (c == 30) {                                   // 跑一半切回 mode 1（join）再切回 3（重新起线程）
                s.set_maint_mode(1); if (s.mthr_.joinable()) { std::printf("  t_mode3 FAIL: 切回 mode 1 线程还在\n"); bad++; }
                bad += cmp_model(s, t, "t_mode3(mid)");
                s.set_maint_mode(3);
            }
        }
        stop = true; rd.join();
        double t0 = now_s(); while (s.maint_pending() && now_s() - t0 < 10) usleep(1000);   // 等后台把活干完
        s.set_maint_mode(1);                                 // join
        auto ms = s.maint_stats();
        if (errs) { std::printf("  t_mode3 FAIL: 读错误 %llu\n", (unsigned long long)errs.load()); bad++; }
        if (ms.units == 0) { std::printf("  t_mode3 FAIL: 没有维护单元跑过\n"); bad++; }
        const u32 bound = (u32)s.lvl_hist().size() * s.lvl_t() * 4 + 4;
        if (peak > bound) { std::printf("  t_mode3 FAIL: run 数峰值 %u 超过兜底线 %u\n", peak, bound); bad++; }
        bad += vok(s, "t_mode3") + cmp_model(s, t, "t_mode3");
        std::printf("  mode 3 后台维护线程 + 读者：读 %llu 次、维护单元 %llu、run 峰值 %u（兜底 %u）、结束 %u 条  %s\n",
                    (unsigned long long)reads.load(), (unsigned long long)ms.units, peak, bound, s.run_count(), bad ? "FAIL" : "ok");
        s.set_maint_mode(3);                                 // 线程活着时析构：~Store 先 join
        for (int i = 0; i < 500; ++i) putm(s, t, 1 + rng() % N, 777, 120);
        if (s.commit() != RC::OK) bad++;
    }
    {   // mode 3 下重新 open（换设备）：open 先停线程、挂载后再起
        static u8 arena2[512 * PAGE];
        Store s2; s2.set_maint_mode(3);
        if (s2.open(&dev, arena2, 512) != RC::OK) { std::printf("  t_mode3 FAIL: reopen\n"); return bad + 1; }
        if (!s2.mthr_.joinable()) { std::printf("  t_mode3 FAIL: open 之后线程没起来\n"); bad++; }
        for (int c = 0; c < 4; ++c) { for (int i = 0; i < 800; ++i) putm(s2, t, 1 + rng() % N, 800 + c, 120); if (s2.commit() != RC::OK) bad++; }
        double t0 = now_s(); while (s2.maint_pending() && now_s() - t0 < 10) usleep(1000);
        s2.set_maint_mode(1);
        bad += vok(s2, "t_mode3(reopen)") + cmp_model(s2, t, "t_mode3(reopen)");
    }
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    alarm(900);
    double t0 = now_s();
    int bad = 0;
    const char* only = argc > 1 ? argv[1] : nullptr;
    auto want = [&](const char* n) { return !only || !std::strcmp(only, n); };
    if (want("stream"))   bad += t_stream();
    if (want("nospace"))  bad += t_stream_nospace();
    if (want("seq64"))    bad += t_seq64();
    if (want("v1chain"))  bad += t_v1chain();
    if (want("cut"))      bad += t_v1chain_cut();
    if (want("merge"))    bad += t_merge_rules();
    if (want("budget"))   bad += t_budget();
    if (want("mode3"))    bad += t_mode3();
    std::printf("g13 第 13 轮：流式产物 + 64 位序号 / v1 清单链升级（第 16 轮改写）+ 层合并规则 + 维护预算 / mode 3: %s (%.1f s)\n", bad ? "FAIL" : "PASS", now_s() - t0);
    return bad ? 1 : 0;
}
