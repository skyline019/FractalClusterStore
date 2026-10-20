// XDB v2 —— 存储层：页缓存 / 写放大计量 / 叶操作 / 内页 / 引擎
//
// 关键不变量（docs/10-design/DESIGN2.md 同步）
//  I1 槽区只追加：4B 单元单调增长，旧 4B 字永不改写 → 无移位、无读-改-写。
//  I2 每页 footer 是「页自身的 redo 记录」{vers, nslots, data_off, rolls, min_key}；
//     崩溃恢复 = 从 footer 截断（内存 nslots 可超前于 footer = 未提交批）。
//  I3 可视性 = ver ≤ view。未提交批 ver = open_ = vers_+1。
//  I4 落盘顺序：值字节 → 值头 → 槽 → 头/尾/页哈希 → 脏字刷出 → 映射页。
//  I5 提交点 = 映射页 state 字的单次 4B 写（A/B 双缓冲轮换）。
//  I6 分配 = 内存空闲表（挂载扫描重建）；快照 pin 住的页永不回收/搬迁。
//  I7 分隔键只降不升（sep ≤ 真最小键），落到相邻页由叶链左右修正兜底。
//  I8 只有可达页参与校验；不可达的旧 COW 副本在挂载时回收。
//  I9 COW 一律「新页 + 父指针换位」，绝不就地重写仍被快照引用的页。
// =============================================================================
//  S00  store.h  —  XDB v2 存储层（页缓存 / 叶操作 / 引擎）
// =============================================================================
#pragma once
#include "common.h"
#include "layout.h"
#include "blockdev.h"
#include <vector>
#include <algorithm>
#include <utility>
#ifdef XDB_ALLOC_TRACE
#include <execinfo.h>
#endif

namespace xdb {

struct Entry {
    u8* b = nullptr;
    u16 pid = 0xFFFF;
    u32 touch = 0;
    u8 valid = 0, dirty = 0, open = 0, locked = 0;
    u32 dbits[PAGE / 128];
};

struct Ent {
    u64 key;
    u16 ver;
    u32 off;
    u16 len;
    const u8* src;
};

enum : u16 { ROOT_PARENT = 0xFFFF, NIL_PAGE = 0xFFFF };
enum : u32 { HORIZON = 8, MAX_SLOTS_E = 400 };

// ------------------------------------------------------------------ S00  ------------------------------------------------------------------ 引擎
class Engine {
public:
    // ---------------------------------------------------------------- 生命周期
    RC open(BlockDev* dev, u8* arena, u32 arena_pages) {
        dev_ = dev;
        arena_ = arena;
        cap_ = arena_pages > NAV_CAP ? NAV_CAP : arena_pages;
        if (cap_ < 4) return RC::INVALID;
        for (u32 i = 0; i < cap_; ++i) {
            ents_[i].b = arena_ + (size_t)i * PAGE;
            ents_[i].pid = 0xFFFF;
            ents_[i].valid = 0;
            ents_[i].dirty = 0;
            ents_[i].open = 0;
            std::memset(ents_[i].dbits, 0, sizeof(ents_[i].dbits));
        }
        clock_ = 1;
        nfree_ = 0;
        nsnap_ = 0;
        nalloc_ = 0;
        pin_.assign(dev->capacity(), 0);
        used_.assign(dev->capacity(), 0);
        oset_.assign(dev->capacity(), 0);
        olist_.clear();
        flog_.clear();
        freestack_.assign(dev->capacity(), 0);
        return mount();
    }

    u16 root() const { return root_; }
    Vers last_commit() const { return vers_; }
    Vers open_vers() const { return open_; }
    DevStats& stats() { return dev_->st; }
    u32 cache_cap() const { return cap_; }
    u32 free_count() const { return nfree_; }

    void touch_open(u16 pid) {
        if (pid < oset_.size() && !oset_[pid]) { oset_[pid] = 1; olist_.push_back(pid); }
    }

    // ---------------------------------------------------------------- 读
    RC get(u64 key, void* out, u16 cap, u16* olen) { return get_at(key, open_, out, cap, olen); }

    RC get_at(u64 key, Vers view, void* out, u16 cap, u16* olen) {
        u16 pid = 0;
        RC rc = find_leaf(key, pid);
        if (rc != RC::OK) return rc;
        Entry* e = load(pid);
        if (!e) return RC::IO;
        return leaf_get(*e, key, view, out, cap, olen);
    }

    template <class F>
    RC scan(F&& fn) {
        u16 pid = leftmost();
        u32 guard = 0;
        while (pid != NIL_PAGE && guard++ < 100000) {
            Entry* e = load(pid);
            if (!e) return RC::IO;
            Ent tmp[MAX_SLOTS_E];
            u32 n = gather_view(*e, tmp, MAX_SLOTS_E, open_, true);
            for (u32 i = 0; i < n; ++i) {
                if (tmp[i].len == 0) continue;
                if (!fn(tmp[i].key, tmp[i].src, tmp[i].len)) return RC::OK;
            }
            LHdr h; lh_read(e->b, h);
            pid = h.next;
        }
        return RC::OK;
    }

    // ---------------------------------------------------------------- 写
    RC put(u64 key, const void* val, u16 len) {
        if (len == 0) len = 1;                    // 0 长度保留给墓碑
        return write_op(key, (const u8*)val, len);
    }
    RC del(u64 key) { return write_op(key, nullptr, 0); }

    RC write_op(u64 key, const u8* val, u16 len) {
        if (dev_->capacity() < 8) return RC::NOSPACE;
        if (len > PAGE - LEAF_DATA0 - 512) return RC::RANGE;
        for (int attempt = 0; attempt < 12; ++attempt) {
            u16 pid = 0;
            RC rc = find_leaf(key, pid);
            if (rc != RC::OK) return rc;
            Entry* e = load(pid);
            if (!e) return RC::FULL;
            LHdr h; lh_read(e->b, h);
            u64 old_min = h.min_key;
            e->open = 1;
            touch_open(pid);
            rc = leaf_insert(*e, key, open_, val, len);
            if (rc == RC::OK) {
                if (key < old_min) {
                    RC r2 = parent_update(pid);
                    if (r2 != RC::OK) return r2;
                }
                return RC::OK;
            }
            if (rc == RC::FULL) {
                RC r2 = relief(pid);
                if (r2 != RC::OK) return r2;
                continue;
            }
            return rc;
        }
        return RC::FULL;
    }

    // ---------------------------------------------------------------- 事务
    RC commit() {
        u32 n = (u32)olist_.size();
        for (u32 i = 0; i < n; ++i) {
            u16 pid = olist_[i];
            if (pid < oset_.size() && !oset_[pid]) continue;   // 提交前已被回收（COW 旧副本）
            Entry* e = load(pid);
            if (!e) return RC::IO;
            RC rc = close_page(*e);
            if (rc != RC::OK) return rc;
        }
        RC rc = flush_all();
        if (rc != RC::OK) return rc;
        rc = write_map((Vers)(open_ + 1));
        if (rc != RC::OK) return rc;
        vers_ = open_;
        open_ = (Vers)(open_ + 1);
        root_c_ = root_;
        finish_txn();
#ifdef XDB_COMMIT_CHECK
        {
            VerifyReport r;
            std::vector<u8> seen(dev_->capacity(), 0);
            verify_node(root_, -1, seen, r, 0);
            u16 p2 = leftmost();
            u16 pv = NIL_PAGE;
            u64 lmax = 0;
            bool have = false;
            while (p2 != NIL_PAGE) {
                Entry* e = load(p2);
                if (!e) break;
                LHdr h; lh_read(e->b, h);
                if (h.prev != pv) { std::fprintf(stderr, "[commit chk] prev link at P%u (ver=%u)\n", p2, vers_); std::abort(); }
                Ent tmp[MAX_SLOTS_E];
                u32 n = gather_view(*e, tmp, MAX_SLOTS_E, open_, true);
                if (have && n && tmp[0].key <= lmax) {
                    std::fprintf(stderr, "[commit chk] order: P%u first=%llu prevmax=%llu (ver=%u)\n", p2,
                                 (unsigned long long)tmp[0].key, (unsigned long long)lmax, vers_);
                    std::abort();
                }
                if (n) { lmax = tmp[n - 1].key; have = true; }
                pv = p2;
                p2 = h.next;
            }
            if (r.errors) {
                std::fprintf(stderr, "[commit chk] verify err=%u %s page=%u (ver=%u)\n", r.errors, r.first_err, r.first_err_page, vers_);
                std::abort();
            }
        }
#endif
        return RC::OK;
    }

    // 依据可达性精确重建 used_/空闲表（abort、回退、挂载后调用）
    void refresh_free() {
        for (u32 p = 0; p < used_.size(); ++p) used_[p] = 0;
        used_[0] = used_[1] = 1;
        walk_pages(root_, [&](u16 pid) { if (pid < used_.size()) used_[pid] = 1; return true; });
        nfree_ = 0;
        for (u32 p = 2; p < used_.size(); ++p) {
            if (used_[p] || pin_[p]) continue;
            freestack_[nfree_++] = (u16)p;
        }
        // 缓存里已不再有效的页：丢弃，避免脏数据写回空闲页
        for (u32 i = 0; i < cap_; ++i) {
            Entry& e = ents_[i];
            if (e.valid && e.pid >= 2 && !used_[e.pid]) { e.valid = 0; e.pid = 0xFFFF; e.dirty = 0; }
        }
    }

    RC abort() {
        root_ = root_c_;                    // 结构回退到上次提交
        u32 n0 = (u32)olist_.size();
        for (u32 i0 = 0; i0 < n0; ++i0) {
            u16 pid = olist_[i0];
            Entry* pe = load(pid);
            if (!pe) continue;
            Entry& e = *pe;
            e.open = 0;
            if (!page_ok(e.b) || !foot_ok(e.b)) { e.valid = 0; continue; }
            Foot f; foot_read(e.b, f);
            wr32(e.b + 4, f.nslots);
            LHdr h; lh_read(e.b, h);
            h.data_off = (u16)f.data_off;
            h.min_key = f.min_key;
            h.b_vers = 0;
            lh_write(e.b, h);
            for (u32 r = 0; r < META_CAP; ++r) {
                Meta m; meta_read(e.b, r, m);
                m.roll = f.rolls[r];
                meta_write(e.b, r, m);
            }
            page_seal(e.b);
            mark(e, 0, PAGE);
        }
        for (u32 i = 0; i < nalloc_; ++i) release_page(alloc_log_[i], "abort");
        nalloc_ = 0;
        flush_all();
        refresh_free();
        finish_txn();
        return RC::OK;
    }

    // ---------------------------------------------------------------- 快照/回退
    RC snapshot() {
        if (nsnap_ >= MAX_SNAP) return RC::FULL;
        Snap& s = snaps_[nsnap_];
        s.vers = vers_;
        s.root = root_;
        s.npages = 0;
        s.nleaves = 0;
        bool overflow = false;
        RC rc = walk_pages(root_, [&](u16 pid) {
            if (s.npages < MAX_PIN) { s.pages[s.npages++] = pid; pin_[pid]++; }
            else overflow = true;
            return true;
        });
        // 记录快照时刻的叶链（回退时按此集合精确重建）
        {
            u16 lp = leftmost();
            u32 guard = 0;
            while (lp != NIL_PAGE && guard++ < MAX_PIN) {
                s.leaves[s.nleaves++] = lp;
                Entry* le = load(lp);
                if (!le) break;
                LHdr lh; lh_read(le->b, lh);
                lp = lh.next;
            }
        }
        if (rc != RC::OK || overflow) {
            for (u32 i = 0; i < s.npages; ++i) pin_[s.pages[i]]--;
            return rc != RC::OK ? rc : RC::FULL;
        }
        ++nsnap_;
        return RC::OK;
    }

    RC rollback(u32 snap_id) {
        if (snap_id >= nsnap_) return RC::INVALID;
        Snap s = snaps_[snap_id];
        RC rc = abort();
        if (rc != RC::OK) return rc;
        root_ = s.root;
        Vers barrier = s.vers;
        Vers hi = max_foot_vers();
        vers_ = hi;
        open_ = (Vers)(vers_ + 1);
        rc = rebuild_from(s, barrier);
        if (rc != RC::OK) return rc;
#ifdef XDB_RB_TRACE
        std::fprintf(stderr, "[rollback] barrier=%u hi=%u open=%u\n", barrier, hi, open_);
#endif
        // 回退后：快照栈清空（被回退消耗），pin 全部释放
        for (u32 i = 0; i < nsnap_; ++i) release_snap(snaps_[i]);
        nsnap_ = 0;
        refresh_free();
        rc = write_map(open_);
        return rc;
    }

    u32 snapshot_count() const { return nsnap_; }
    Vers snapshot_vers(u32 i) const { return i < nsnap_ ? snaps_[i].vers : 0; }

    // 调试：叶链与树的可达叶集合是否一致
    bool debug_chain_tree(std::vector<u16>& missing, std::vector<u16>& extra) {
        std::vector<u8> reach(dev_->capacity(), 0);
        walk_pages(root_, [&](u16 pid) {
            Entry* e = load(pid);
            if (e && page_kind(e->b) == PK_LEAF && pid < reach.size()) reach[pid] = 1;
            return true;
        });
        std::vector<u8> inchain(dev_->capacity(), 0);
        u16 pid = leftmost();
        u32 guard = 0;
        while (pid != NIL_PAGE && guard++ < 100000 && pid < inchain.size()) {
            inchain[pid] = 1;
            Entry* e = load(pid);
            if (!e) break;
            LHdr h; lh_read(e->b, h);
            pid = h.next;
        }
        missing.clear(); extra.clear();
        for (u32 p = 2; p < dev_->capacity(); ++p) {
            if (reach[p] && !inchain[p]) missing.push_back((u16)p);
            if (!reach[p] && inchain[p]) extra.push_back((u16)p);
        }
        return missing.empty() && extra.empty();
    }

    void debug_dump_events() { dump_ev(); }

    // 调试：used_ 中既不可达、也不在叶链上的页（泄漏）
    void debug_leaks(std::vector<u16>& out) {
        std::vector<u8> mark(dev_->capacity(), 0);
        walk_pages(root_, [&](u16 pid) { if (pid < mark.size()) mark[pid] = 1; return true; });
        u16 pid = leftmost();
        u32 guard = 0;
        while (pid != NIL_PAGE && guard++ < 100000 && pid < mark.size()) {
            mark[pid] = 1;
            Entry* e = load(pid);
            if (!e) break;
            LHdr h; lh_read(e->b, h);
            pid = h.next;
        }
        out.clear();
        for (u32 p = 2; p < dev_->capacity(); ++p)
            if (used_[p] && !mark[p]) out.push_back((u16)p);
    }

    // ---------------------------------------------------------------- 校验
    struct VerifyReport {
        u32 pages = 0, leaves = 0, inners = 0, pubs = 0, orphans = 0, errors = 0;
        u16 first_err_page = 0;
        const char* first_err = "";
    };

    RC verify(VerifyReport* rep = nullptr) {
        VerifyReport r;
        std::vector<u8> seen(dev_->capacity(), 0);
        verify_node(root_, -1, seen, r, 0);
        u16 pid = leftmost();
        u16 prev = NIL_PAGE;
        u64 last_max = 0;
        bool have_last = false;
        u32 guard = 0;
        while (pid != NIL_PAGE && guard++ < 100000) {
            Entry* e = load(pid);
            if (!e) { rerr(r, pid, "leaf load"); break; }
            LHdr h; lh_read(e->b, h);
            if (h.prev != prev) rerr(r, pid, "prev link");
            Ent tmp[MAX_SLOTS_E];
            u32 n = gather_view(*e, tmp, MAX_SLOTS_E, open_, true);
            for (u32 i = 0; i < n; ++i) {
                if (tmp[i].len == 0) continue;
                if (i && tmp[i].key <= tmp[i - 1].key) rerr(r, pid, "keys not ascending");
                if (have_last && tmp[i].key <= last_max) rerr(r, pid, "key below prev leaf");
            }
            if (n && tmp[n - 1].len) { last_max = tmp[n - 1].key; have_last = true; }
            prev = pid;
            pid = h.next;
        }
        for (u32 p = 2; p < dev_->capacity(); ++p)
            if (used_[p] && !seen[p]) {
                ++r.orphans;
#ifdef XDB_ORPHAN_TRACE
                Entry* oe = load((u16)p);
                const u8* ob = oe ? oe->b : nullptr;
                u32 okind = ob ? page_kind(ob) : 0xFF;
                u32 ons = ob ? rd32(ob + 4) : 0;
                u64 omin = 0; u16 onid = 0, onext = 0, oprev = 0;
                if (ob && okind == PK_LEAF) { LHdr oh; lh_read(ob, oh); omin = oh.min_key; onid = oh.nid; onext = oh.next; oprev = oh.prev; }
                std::fprintf(stderr, "[orphan] P%u kind=%u ns=%u min=%llu nid=%u next=%u prev=%u\n", p, okind, ons,
                             (unsigned long long)omin, onid, onext, oprev);
                {
                    u32 cnt = ben_ < 128 ? ben_ : 128;
                    for (u32 q = 0; q < cnt; ++q) {
                        const BE& b = be_[(ben_ - cnt + q) % 128];
                        if (b.pid == p) std::fprintf(stderr, "    built seq=%u n=%u k0=%llu k1=%llu\n", b.seq, b.n,
                                                     (unsigned long long)b.k0, (unsigned long long)b.k1);
                    }
                }
#endif
            }
        if (rep) *rep = r;
        return r.errors ? RC::CORRUPT : RC::OK;
    }

private:
    static const u32 MAX_PAGES = 65536;
    static const u32 MAX_SNAP = 8;
    static const u32 MAX_PIN = 4096;

    struct Snap {
        Vers vers;
        u16 root;
        u32 npages;
        u16 pages[MAX_PIN];
        u32 nleaves;
        u16 leaves[MAX_PIN];
    };

    // 事件环（调试）
    struct Ev { u8 c; u16 a, b; };
    Ev ev_[96];
    u32 evn_ = 0;
    void set_ev(u8 c, u16 a, u16 b = 0xFFFF) { ev_[evn_ % 96] = Ev{c, a, b}; ++evn_; }
    struct BE { u32 seq; u16 pid; u16 n; u64 k0, k1; };
    BE be_[128];
    u32 ben_ = 0;
    void log_build(u16 pid, u32 n, u64 k0, u64 k1) { be_[ben_ % 128] = BE{ben_, pid, (u16)n, k0, k1}; ++ben_; }
    void dump_ev() {
        u32 cnt = evn_ < 96 ? evn_ : 96;
        for (u32 i = 0; i < cnt; ++i) {
            const Ev& e = ev_[(evn_ - cnt + i) % 96];
            std::fprintf(stderr, "  ev %c %u %u\n", e.c, e.a, e.b);
        }
    }

    // ================================================================ 缓存
    Entry* find(u16 pid) {
        for (u32 i = 0; i < cap_; ++i)
            if (ents_[i].valid && ents_[i].pid == pid) return &ents_[i];
        return nullptr;
    }

    Entry* victim() {
        // 优先换出已提交页；未提交页只在必要时换出（其未提交批会先落盘，
        // 重载不截断，abort/挂载都按 footer 回退，因此是安全的）。
        Entry* best = nullptr;
        u32 bmin = 0xFFFFFFFFu;
        for (u32 i = 0; i < cap_; ++i) {
            Entry& e = ents_[i];
            if (!e.valid) return &e;
            if (e.locked) continue;
            if (!e.open && e.touch < bmin) { bmin = e.touch; best = &e; }
        }
        if (best) return best;
        bmin = 0xFFFFFFFFu;
        for (u32 i = 0; i < cap_; ++i) {
            Entry& e = ents_[i];
            if (e.locked) continue;
            if (e.touch < bmin) { bmin = e.touch; best = &e; }
        }
        return best;
    }

    RC evict(Entry& e) {
        if (!e.valid) return RC::OK;
        if (e.dirty) {
            RC rc = flush_page(e);
            if (rc != RC::OK) return rc;
        }
        e.valid = 0;
        e.pid = 0xFFFF;
        return RC::OK;
    }

    Entry* load(u16 pid) {
        Entry* e = find(pid);
        if (e) { e->touch = clock_++; return e; }
        Entry* v = victim();
        if (!v) return nullptr;
        if (evict(*v) != RC::OK) return nullptr;
        std::memset(v->b, 0, PAGE);
        if (dev_->read(pid, 0, v->b, PAGE) != RC::OK) return nullptr;
        v->pid = pid;
        v->valid = 1;
        v->dirty = 0;
        v->open = 0;
        std::memset(v->dbits, 0, sizeof(v->dbits));
        v->touch = clock_++;
        return v;
    }

    Entry* make(u16 pid) {
        Entry* v = find(pid);
        if (v) { v->valid = 1; v->open = 0; }
        else {
            v = victim();
            if (!v) return nullptr;
            if (evict(*v) != RC::OK) return nullptr;
            v->pid = pid;
            v->valid = 1;
            v->open = 0;
            v->touch = clock_++;
        }
        v->dirty = 0;
        std::memset(v->dbits, 0, sizeof(v->dbits));
        std::memset(v->b, 0, PAGE);
        mark(*v, 0, PAGE);
        return v;
    }

    void mark(Entry& e, u32 off, u32 len) {
        u32 w0 = off / 4, w1 = (off + len + 3) / 4;
        for (u32 w = w0; w < w1; ++w) e.dbits[w >> 5] |= (1u << (w & 31));
        e.dirty = 1;
        touch_open(e.pid);
    }

    void wbytes(Entry& e, u32 off, const void* src, u32 len) {
        std::memcpy(e.b + off, src, len);
        mark(e, off, len);
    }

    RC flush_page(Entry& e) {
        u32 w = 0;
        while (w < PAGE / 4) {
            if (!(e.dbits[w >> 5] & (1u << (w & 31)))) { ++w; continue; }
            u32 start = w;
            while (w < PAGE / 4) {
                if (!(e.dbits[w >> 5] & (1u << (w & 31)))) {
                    u32 k = w;
                    while (k < PAGE / 4 && !(e.dbits[k >> 5] & (1u << (k & 31)))) ++k;
                    if (k >= PAGE / 4 || k - w > 8) break;
                    w = k;
                }
                ++w;
            }
            RC rc = dev_->write(e.pid, start * 4, e.b + start * 4, (w - start) * 4);
            if (rc != RC::OK) return rc;
        }
        std::memset(e.dbits, 0, sizeof(e.dbits));
        e.dirty = 0;
        return RC::OK;
    }

    RC flush_all() {
        for (u32 i = 0; i < cap_; ++i)
            if (ents_[i].valid && ents_[i].dirty) {
                RC rc = flush_page(ents_[i]);
                if (rc != RC::OK) return rc;
            }
        return RC::OK;
    }

    void drop(u16 pid) {
        Entry* e = find(pid);
        if (!e) return;
        e->valid = 0;
        e->pid = 0xFFFF;
        e->dirty = 0;
    }

    // ================================================================ 分配
    RC alloc_page(u16& out) {
        while (nfree_) {
            u16 p = freestack_[--nfree_];
            if (p >= dev_->capacity() || p < 2) continue;
            if (pin_[p]) continue;
            if (used_[p]) continue;          // 空闲表里的陈旧条目
#ifdef XDB_ASSERT_SORTED
            if (used_[p]) { std::fprintf(stderr, "[alloc] page %u already used!\n", p); std::abort(); }
#endif
            drop(p);
            out = p;
#ifdef XDB_ALLOC_TRACE
            std::fprintf(stderr, "[alloc] P%u\n", p);
#endif
            set_ev('a', p);
            used_[p] = 1;
            if (nalloc_ < 64) alloc_log_[nalloc_++] = p;
            return RC::OK;
        }
        if ((u32)(hi_ + 1) < dev_->capacity()) {
            out = hi_++;
            drop(out);
#ifdef XDB_ALLOC_TRACE
            std::fprintf(stderr, "[alloc-new] P%u\n", out);
#endif
            set_ev('n', out);
            used_[out] = 1;
            if (nalloc_ < 64) alloc_log_[nalloc_++] = out;
            return RC::OK;
        }
        return RC::NOSPACE;
    }

    void release_page(u16 pid, const char* tag = "?") {
        if (pid < 2 || pid >= dev_->capacity()) return;
        if (pin_[pid]) return;
#ifdef XDB_ASSERT_SORTED
        if (!used_[pid]) { std::fprintf(stderr, "[free] page %u already free! (tag=%s)\n", pid, tag); }
#endif
        (void)tag;
        if (!used_[pid]) return;          // 幂等：同事务内已被回收的页不再重复入表
        flog_.push_back(pid);
#ifdef XDB_ALLOC_TRACE
        std::fprintf(stderr, "[free:%s] P%u\n", tag, pid);
#endif
        set_ev('f', pid);
        if (nfree_ < freestack_.size()) freestack_[nfree_++] = pid;
        used_[pid] = 0;
        if (pid < oset_.size()) oset_[pid] = 0;   // 已回收页不得再参与提交收尾
        drop(pid);
    }

    // ================================================================ 挂载
    RC mount() {
        MapRec ma, mb;
        bool oka = false, okb = false;
        u8* buf = arena_;
        if (dev_->read(MAP_A, 0, buf, PAGE) == RC::OK && page_ok(buf) && page_kind(buf) == PK_MAP) {
            map_read(buf, ma);
            oka = (ma.state == 0xA55A && rd32(buf + MAP_REC + 4) == MAGIC_MAP);
        }
        if (dev_->read(MAP_B, 0, buf, PAGE) == RC::OK && page_ok(buf) && page_kind(buf) == PK_MAP) {
            map_read(buf, mb);
            okb = (mb.state == 0xA55A && rd32(buf + MAP_REC + 4) == MAGIC_MAP);
        }
        if (!oka && !okb) return format();
        MapRec m = (!oka) ? mb : (!okb) ? ma : (ma.next_vers >= mb.next_vers ? ma : mb);
        root_ = m.root;
        root_c_ = root_;
        ring_ = m;
        vers_ = (Vers)(m.next_vers ? m.next_vers - 1 : 0);
        open_ = m.next_vers ? m.next_vers : 1;
        used_.assign(dev_->capacity(), 0);
        used_[0] = used_[1] = 1;
        u16 maxused = 1;
        Vers maxfv = 0;
        for (u32 p = 2; p < dev_->capacity(); ++p) {
            if (dev_->read((u16)p, 0, buf, PAGE) != RC::OK) continue;
            if (!page_ok(buf)) continue;
            u8 k = page_kind(buf);
            if (k != PK_LEAF && k != PK_INNER) continue;
            used_[p] = 1;
            maxused = (u16)p;
            if (!foot_ok(buf)) continue;
            Foot f; foot_read(buf, f);
            if (f.vers > maxfv) maxfv = f.vers;
            // 崩溃恢复：把页截断到 footer 记录的已提交状态
            if (rd32(buf + 4) != f.nslots) {
                wr32(buf + 4, f.nslots);
                if (k == PK_LEAF) {
                    LHdr h; lh_read(buf, h);
                    h.data_off = (u16)f.data_off;
                    h.min_key = f.min_key;
                    h.b_vers = 0;
                    h.close_vers = f.vers;
                    lh_write(buf, h);
                    for (u32 r = 0; r < META_CAP; ++r) {
                        Meta m; meta_read(buf, r, m);
                        m.roll = f.rolls[r];
                        meta_write(buf, r, m);
                    }
                }
                page_seal(buf);
                dev_->write((u16)p, 0, buf, LEAF_DATA0);
            }
        }
        hi_ = (u16)(maxused + 1);
        // 空闲表 + 可达集合
        std::vector<u8> reach(dev_->capacity(), 0);
        walk_pages(root_, [&](u16 pid) { if (pid < reach.size()) reach[pid] = 1; return true; });
        nfree_ = 0;
        for (u32 p = 2; p < dev_->capacity(); ++p) {
            if (!used_[p] || reach[p]) continue;
            used_[p] = 0;
            if (nfree_ < freestack_.size()) freestack_[nfree_++] = (u16)p;
        }
        if (maxfv > vers_) { vers_ = maxfv; open_ = (Vers)(maxfv + 1); }
        return RC::OK;
    }

    RC format() {
        used_.assign(dev_->capacity(), 0);
        used_[0] = used_[1] = 1;
        nfree_ = 0;
        hi_ = 2;
        u16 leaf = 0;
        RC rc = alloc_page(leaf);
        if (rc != RC::OK) return rc;
        Ent ents[1];
        ents[0] = Ent{0, 0, 0, 0, nullptr};
        rc = build_leaf(leaf, ents, 0, ROOT_PARENT, NIL_PAGE, NIL_PAGE, 0);
        if (rc != RC::OK) return rc;
        root_ = leaf;
        root_c_ = root_;
        vers_ = 0;
        open_ = 1;
        memset(&ring_, 0, sizeof(ring_));
        ring_.nid = 1;
        rc = write_map(1, 1);
        if (rc != RC::OK) return rc;
        rc = write_map(1, 0);
        if (rc != RC::OK) return rc;
        return RC::OK;
    }

    RC write_map(Vers next, int which = -1) {
        MapRec m = ring_;
        m.state = 0xA55A;
        m.root = root_;
        m.next_vers = next;
        m.acc = (u16)((m.acc + 1) & 0xFFFF);
        m.ring[next & 63] = root_;
        m.batch_n = (u16)nalloc_;
        ring_ = m;
        int target = (which >= 0) ? which : (int)(next & 1);
        Entry* e = find((u16)target);
        if (!e) e = make((u16)target);
        if (!e) return RC::IO;
        u8* b = e->b;
        b[0] = PK_MAP;
        map_write(b, m);
        // 先以 state=0 落盘，再写 state 字（单次 4B = 提交点）
        u16 saved = m.state;
        page_seal(b);                    // 哈希覆盖最终镜像（含生效 state）
        wr16(b + MAP_REC + 8, 0);        // 先落「未生效」镜像
        // 只标记变化的字段：状态/根/计数/版本 + 历史环
        mark(*e, 0, 12);
        mark(*e, MAP_REC + 8, 16);
        u32 ro = MAP_REC + 24 + 2 * CAND_CAP + 2 * (next & 63);
        mark(*e, ro & ~3u, 8);
        RC rc = flush_page(*e);
        if (rc != RC::OK) return rc;
        wr16(b + MAP_REC + 8, saved);
        mark(*e, MAP_REC + 8, 4);
        rc = flush_page(*e);
        used_[target] = 1;
        return rc;
    }

    // ================================================================ 槽
    u32 collect_pubs(const Entry& e, u32* pos, u32 cap) {
        u32 nslots = rd32(e.b + 4);
        u32 unit = 0, n = 0;
        while (unit < nslots && n < cap) {
            u32 off = slot_off(unit);
            u8 kind = e.b[off];
            u32 u = slot_units(kind);
            if (unit + u > nslots) break;
            if (kind == SK_PUB) pos[n++] = off;
            unit += u;
        }
        return n;
    }

    RC leaf_get(Entry& e, u64 key, Vers view, void* out, u16 cap, u16* olen) {
        LHdr h; lh_read(e.b, h);
        if (key < h.min_key) return RC::NOTFOUND;
        u32 pos[MAX_SLOTS_E];
        u32 n = collect_pubs(e, pos, MAX_SLOTS_E);
        for (u32 i = n; i-- > 0;) {
            Pub p;
            if (!pub_at(e.b, pos[i], p)) continue;
            if (p.key != key) continue;
            if (p.ver > view) continue;
            if (p.r_off == 0) return RC::NOTFOUND;
            u16 len = rd16(e.b + p.r_off + 2);
            if (!valhdr_ok(e.b, p.r_off, len)) return corrupt_("valhdr");
            if (len > cap) return RC::RANGE;
            std::memcpy(out, e.b + p.r_off + VALHDR_SZ, len);
            if (olen) *olen = len;
            return RC::OK;
        }
        return RC::NOTFOUND;
    }

    RC leaf_insert(Entry& e, u64 key, u16 ver, const u8* val, u16 len) {
        LHdr h; lh_read(e.b, h);
        u32 nslots = rd32(e.b + 4);
        u32 pos[MAX_SLOTS_E];
        u32 n = collect_pubs(e, pos, MAX_SLOTS_E);
        u32 chain = 0, region = 0xFFFFFFFFu;
        bool exists = false;
        for (u32 i = n; i-- > 0;) {
            Pub p;
            if (!pub_at(e.b, pos[i], p)) continue;
            if (p.key != key) continue;
            exists = true;
            ++chain;
            region = p.cred >> 26;
        }
        if (!exists) region = (u32)(mix32((u32)key ^ (u32)(key >> 32)) % META_CAP);
        u32 need_val = len ? (u32)len + VALHDR_SZ : 0;
        u32 slot_end = LEAF_DATA0 + 4 * (nslots + PUB_SZ / 4);
        if ((u32)h.data_off < slot_end + need_val) return RC::FULL;
        u32 r_off = 0;
        if (len) {
            r_off = (u32)h.data_off - need_val;
            wbytes(e, r_off + VALHDR_SZ, val, len);
            u8 hd[VALHDR_SZ];
            hd[0] = SK_VALHDR;
            hd[1] = (u8)atom_hash((const u8*)&len, 2, 0x91u);
            wr16(hd + 2, len);
            wbytes(e, r_off, hd, VALHDR_SZ);
            h.data_off = (u16)r_off;
        }
        u32 cred = (region << 26) | (chain + 1);
        u8 sb[PUB_SZ];
        sb[0] = SK_PUB;
        sb[1] = pub_chk(SK_PUB, ver, cred, key, r_off);
        wr16(sb + 2, ver);
        wr32(sb + 4, cred);
        wr64(sb + 8, key);
        wr32(sb + 16, r_off);
        wbytes(e, slot_off(nslots), sb, PUB_SZ);
        wr32(e.b + 4, nslots + PUB_SZ / 4);
        mark(e, 4, 4);
        if (key < h.min_key || h.min_key == 0) h.min_key = (key < h.min_key) ? key : key;
        lh_write(e.b, h);
        mark(e, LEAF_HDR, 40);
        e.dirty = 1;
        return RC::OK;
    }

    // 收集「截至 view 的每键条目」：最新版本必留；≥ keep_from 的旧版本按需保留（MVCC/快照）
    u32 gather_view(Entry& e, Ent* out, u32 cap, Vers view, bool drop_tombs, Vers keep_from = 0xFFFF) {
        u32 pos[MAX_SLOTS_E];
        u32 n = collect_pubs(e, pos, MAX_SLOTS_E);
        u32 cnt = 0;
        for (u32 i = n; i-- > 0;) {                     // 由新到旧
            Pub p;
            if (!pub_at(e.b, pos[i], p)) continue;
            if (p.ver > view) continue;
            if (cnt >= cap) break;
            if (p.r_off == 0) {
                out[cnt++] = Ent{p.key, p.ver, 0, 0, nullptr};
            } else {
                if (p.r_off + VALHDR_SZ > SLOT_CUR0) continue;
                u16 len = rd16(e.b + p.r_off + 2);
                if (!valhdr_ok(e.b, p.r_off, len)) continue;
                out[cnt++] = Ent{p.key, p.ver, (u32)(p.r_off + VALHDR_SZ), len, e.b + p.r_off + VALHDR_SZ};
            }
        }
        // 键升序、版本降序（稳定：同 (key,ver) 保留物理更晚者）
        for (u32 i = 1; i < cnt; ++i) {
            Ent v = out[i];
            u32 j = i;
            while (j > 0 && (out[j - 1].key > v.key || (out[j - 1].key == v.key && out[j - 1].ver < v.ver))) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = v;
        }
        // 每组：去重 (key,ver)；最新是墓碑 → 只留墓碑；否则留最新 + 版本 ≥ keep_from
        u32 w = 0;
        for (u32 i = 0; i < cnt;) {
            u32 j = i + 1;
            while (j < cnt && out[j].key == out[i].key) ++j;
            // 本组 [i, j)：i 是最新
            bool tomb = (out[i].len == 0);
            if (tomb) {
                if (!drop_tombs) out[w++] = out[i];
            } else {
                u16 prev_ver = 0xFFFF;
                bool firstone = true;
                for (u32 k = i; k < j; ++k) {
                    if (!firstone && out[k].ver == prev_ver) continue;    // 同 (key,ver) 重复
                    if (!firstone && out[k].ver < keep_from) continue;    // 过老版本
                    out[w++] = out[k];
                    prev_ver = out[k].ver;
                    firstone = false;
                }
            }
            i = j;
        }
        // 键升序、版本升序（供 build_leaf 计算链位置）
        for (u32 i = 1; i < w; ++i) {
            Ent v = out[i];
            u32 j = i;
            while (j > 0 && (out[j - 1].key > v.key || (out[j - 1].key == v.key && out[j - 1].ver > v.ver))) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = v;
        }
        return w;
    }

    static void sort_ents(Ent* a, u32 n) {
        for (u32 i = 1; i < n; ++i) {
            Ent v = a[i];
            u32 j = i;
            while (j > 0 && a[j - 1].key > v.key) { a[j] = a[j - 1]; --j; }
            a[j] = v;
        }
    }

    RC build_leaf(u16 pid, Ent* ents, u32 n, u16 nid, u16 next, u16 prev, Vers vers, u64 min_hint = 0) {
#ifdef XDB_KTRACE
        for (u32 i = 0; i < n; ++i)
            if (ents[i].key == XDB_KTRACE)
                std::fprintf(stderr, "[build] pid=%u k=%llu ver=%u len=%u\n", pid, (unsigned long long)ents[i].key, ents[i].ver, ents[i].len);
#endif
        {
            u32 need = 0;
            for (u32 i = 0; i < n; ++i) need += PUB_SZ + (ents[i].len ? (u32)ents[i].len + VALHDR_SZ : 0);
            if (LEAF_DATA0 + need + 64 > SLOT_CUR0) return RC::INVALID;   // 先判容量，绝不破坏页
        }
        log_build(pid, n, n ? ents[0].key : 0, n ? ents[n - 1].key : 0);
        Entry* e = make(pid);
        if (!e) return RC::IO;
        page_init(e->b, PK_LEAF);
        anchor_write(e->b, pid, 0);
        LHdr h;
        h.nid = nid;
        h.min_key = n ? ents[0].key : min_hint;
        if (min_hint && min_hint < h.min_key) h.min_key = min_hint;
        h.next = next;
        h.prev = prev;
        h.meta_cnt = META_CAP;
        h.meta_cap = META_CAP;
        h.data_off = (u16)SLOT_CUR0;
        h.close_vers = vers;
        h.b_vers = 0;
        h.maxu = 0;
        h.flags2 = 0;
        lh_write(e->b, h);
        wr32(e->b + 4, 0);
        for (u32 r = 0; r < META_CAP; ++r) {
            Meta m; m.min_key = 0; m.roll = 0; m.start = 0; m.cnt = 0;
            meta_write(e->b, r, m);
        }
        u32 unit = 0, data_cur = SLOT_CUR0;
        u64 cur_key = 0;
        u32 chain = 0;
        for (u32 i = 0; i < n; ++i) {
            u32 region = n ? (u32)((u64)i * META_CAP / n) : 0;
            if (region >= META_CAP) region = META_CAP - 1;
            if (i == 0 || ents[i].key != cur_key) { cur_key = ents[i].key; chain = 0; }
            ++chain;
            u32 cred = (region << 26) | chain;
            u32 r_off = 0;
            if (ents[i].len) {
                data_cur -= (u32)ents[i].len + VALHDR_SZ;
                std::memcpy(e->b + data_cur + VALHDR_SZ, ents[i].src, ents[i].len);
                u8 hd[VALHDR_SZ];
                hd[0] = SK_VALHDR;
                hd[1] = (u8)atom_hash((const u8*)&ents[i].len, 2, 0x91u);
                wr16(hd + 2, ents[i].len);
                std::memcpy(e->b + data_cur, hd, VALHDR_SZ);
                r_off = data_cur;
            }
            u8 sb[PUB_SZ];
            sb[0] = SK_PUB;
            sb[1] = pub_chk(SK_PUB, ents[i].ver, cred, ents[i].key, r_off);
            wr16(sb + 2, ents[i].ver);
            wr32(sb + 4, cred);
            wr64(sb + 8, ents[i].key);
            wr32(sb + 16, r_off);
            std::memcpy(e->b + slot_off(unit), sb, PUB_SZ);
            unit += PUB_SZ / 4;
            Meta m; meta_read(e->b, region, m);
            if (m.cnt == 0) m.min_key = ents[i].key;
            m.cnt++;
            meta_write(e->b, region, m);
        }
        // 值区与槽区冲突检查
        if (LEAF_DATA0 + 4 * unit > data_cur) return RC::INVALID;
        wr32(e->b + 4, unit);
        h.data_off = (u16)data_cur;
        h.min_key = n ? ents[0].key : min_hint;
        if (min_hint && min_hint < h.min_key) h.min_key = min_hint;
        lh_write(e->b, h);
        Foot f;
        f.vers = vers;
        f.nslots = unit;
        f.data_off = data_cur;
        f.min_key = h.min_key;
        for (u32 r = 0; r < META_CAP; ++r) {
            Meta m; meta_read(e->b, r, m);
            f.rolls[r] = m.roll;
        }
        foot_write_prefix(e->b, f);
        foot_seal(e->b);
        e->open = 1;
        e->dirty = 1;
        return RC::OK;
    }

    RC close_page(Entry& e) {
#ifdef XDB_ALLOC_TRACE
        if (rd32(e.b + 4) == 0 && page_kind(e.b) == PK_LEAF) {
            std::fprintf(stderr, "[close-empty] P%u kind=%u\n", e.pid, page_kind(e.b));
            dump_ev();
        }
#endif
        u8 k = page_kind(e.b);
        if (k == PK_LEAF) {
            LHdr h; lh_read(e.b, h);
            Foot f;
            f.vers = open_;
            f.nslots = rd32(e.b + 4);
            f.data_off = h.data_off;
            f.min_key = h.min_key;
            for (u32 r = 0; r < META_CAP; ++r) f.rolls[r] = 0;
            u32 pos[MAX_SLOTS_E];
            u32 n = collect_pubs(e, pos, MAX_SLOTS_E);
            for (u32 i = 0; i < n; ++i) {
                Pub p;
                if (!pub_at(e.b, pos[i], p)) continue;
                u32 region = p.cred >> 26;
                if (region >= META_CAP) continue;
                u32 hh = mix32((u32)p.key ^ (u32)(p.key >> 32));
                u32 vh;
                if (p.r_off) {
                    u16 len = rd16(e.b + p.r_off + 2);
                    vh = atom_hash(e.b + p.r_off + VALHDR_SZ, len, SEED_VALS);
                } else vh = 0xDEADBEEFu;
                f.rolls[region] += mix32(hh ^ vh ^ mix32(p.ver));
            }
            for (u32 r = 0; r < META_CAP; ++r) {
                Meta m; meta_read(e.b, r, m);
                m.roll = f.rolls[r];
                meta_write(e.b, r, m);
            }
            h.close_vers = open_;
            h.b_vers = 0;
            lh_write(e.b, h);
            foot_write_prefix(e.b, f);
            foot_seal(e.b);
        } else if (k == PK_INNER) {
            IHdr h; ih_read(e.b, h);
            Foot f;
            f.vers = open_;
            f.nslots = rd32(e.b + 4);
            f.data_off = 0;
            f.min_key = 0;
            for (u32 r = 0; r < META_CAP; ++r) f.rolls[r] = 0;
            h.b_vers = 0;
            ih_write(e.b, h);              // 先改头，再盖哈希
            foot_write_prefix(e.b, f);
            foot_seal(e.b);
        } else {
            page_seal(e.b);
        }
        e.dirty = 1;
        mark(e, 0, LEAF_DATA0);      // 头/链群摘要/footer/页哈希 共 400B
        return RC::OK;
    }

    // ================================================================ 查找
    u16 leftmost() {
        u16 pid = root_;
        for (int d = 0; d < 64; ++d) {
            Entry* e = load(pid);
            if (!e) return NIL_PAGE;
            if (page_kind(e->b) == PK_LEAF) {
                LHdr h; lh_read(e->b, h);
                if (h.prev == NIL_PAGE) return pid;
                pid = h.prev;
                continue;
            }
            IHdr h; ih_read(e->b, h);
            if (h.nslots == 0) return NIL_PAGE;
            ISlot s;
            if (!islot_ok(e->b, 0, s)) return NIL_PAGE;
            pid = s.child;
        }
        return NIL_PAGE;
    }

    RC find_leaf(u64 key, u16& out) {
        u16 pid = root_;
        for (int depth = 0; depth < MAX_LEVEL * 4; ++depth) {
            Entry* e = load(pid);
            if (!e) return RC::IO;
            u8 k = page_kind(e->b);
            if (k == PK_LEAF) { out = pid; return leaf_fix(key, out); }
            if (k != PK_INNER) return corrupt_("find_leaf kind");
            IHdr h; ih_read(e->b, h);
            if (h.nslots == 0) return corrupt_("find_leaf nslots", pid);
            u32 lo = 0, hi = h.nslots;
            while (lo + 1 < hi) {
                u32 mid = (lo + hi) / 2;
                ISlot s;
                if (!islot_ok(e->b, mid, s)) return corrupt_("find_leaf bsearch");
                if (s.key <= key) lo = mid; else hi = mid;
            }
            ISlot s;
            if (!islot_ok(e->b, lo, s)) return corrupt_("find_leaf lo");
            pid = s.child;
        }
        return corrupt_("find_leaf depth");
    }

    RC leaf_fix(u64 key, u16& pid) {
        for (int step = 0; step < 16; ++step) {
            Entry* e = load(pid);
            if (!e) return RC::IO;
            LHdr h; lh_read(e->b, h);
            if (key < h.min_key && h.prev != NIL_PAGE) { pid = h.prev; continue; }
            if (h.next != NIL_PAGE) {
                u16 nx = h.next;
                Entry* ne = load(nx);
                if (!ne) return RC::IO;
                LHdr nh; lh_read(ne->b, nh);
                if (key >= nh.min_key) { pid = nx; continue; }
            }
            return RC::OK;
        }
        return RC::OK;
    }

    // ================================================================ 救济
    RC relief(u16 pid) {
        Entry* e = load(pid);
        if (!e) return RC::IO;
        // 若有可丢弃的旧版本 → settle；否则直接分裂
        LHdr h; lh_read(e->b, h);
        u32 pos[MAX_SLOTS_E];
        u32 npub = collect_pubs(*e, pos, MAX_SLOTS_E);
        Ent tmp[MAX_SLOTS_E];
        u32 live = gather_view(*e, tmp, MAX_SLOTS_E, open_, true, history_floor());
        // 估算压缩后是否放得下
        u32 bytes = 0;
        for (u32 i = 0; i < live; ++i) bytes += (u32)tmp[i].len + VALHDR_SZ;
        u32 need = LEAF_DATA0 + 4u * (live * (PUB_SZ / 4) + 4) + bytes;
        bool can_settle = (npub > live) && (need + 256 < (u32)h.data_off + 4096) && !pin_[pid];
        if (can_settle) {
            RC rc = settle(pid);
            if (rc == RC::OK) return RC::OK;
        }
        return split_leaf(pid);
    }

    RC settle(u16 pid, bool newest_only = false) {
        Entry* e = load(pid);
        if (!e) return RC::IO;
        if (pin_[pid]) return RC::BUSY;
        LHdr h; lh_read(e->b, h);
        Ent tmp[MAX_SLOTS_E];
        u32 n = gather_view(*e, tmp, MAX_SLOTS_E, open_, true, newest_only ? (Vers)0xFFFF : history_floor());
#ifdef XDB_ALLOC_TRACE
        if (n == 0) std::fprintf(stderr, "[settle-empty] P%u\n", pid);
#endif
        u32 bytes = 0;
        for (u32 i = 0; i < n; ++i) bytes += (u32)tmp[i].len + VALHDR_SZ;
        if (LEAF_DATA0 + 4u * (n * (PUB_SZ / 4) + 4) + bytes + 64 > SLOT_CUR0) return RC::INVALID;
        u16 np = 0;
        RC rc = alloc_page(np);
        if (rc != RC::OK) return rc;
        static thread_local u8 stage[PAGE];
        u32 sp = 0;
        Ent ents[MAX_SLOTS_E];
        for (u32 i = 0; i < n; ++i) {
            ents[i] = tmp[i];
            if (tmp[i].len) {
                if (sp + tmp[i].len > PAGE) { release_page(np, "settle-bail"); return RC::INVALID; }
                std::memcpy(stage + sp, tmp[i].src, tmp[i].len);
                ents[i].src = stage + sp;
                sp += tmp[i].len;
            } else ents[i].src = nullptr;
        }
        u16 next = h.next, prev = h.prev, nid = h.nid;
        rc = build_leaf(np, ents, n, nid, next, prev, open_, h.min_key);
        if (rc != RC::OK) { release_page(np, "settle-bail"); return rc; }
#ifdef XDB_SETTLE_CHECK
        {
            Entry* chk = load(np);
            Ent tmp2[MAX_SLOTS_E];
            u32 n2 = gather_view(*chk, tmp2, MAX_SLOTS_E, open_, true, history_floor());
            if (n2 != n) {
                std::fprintf(stderr, "[settle mismatch] pid=%u wrote=%u read=%u\n", np, n, n2);
                for (u32 i = 0; i < n; ++i) std::fprintf(stderr, "  wrote k=%llu ver=%u len=%u\n", (unsigned long long)ents[i].key, ents[i].ver, ents[i].len);
                for (u32 i = 0; i < n2; ++i) std::fprintf(stderr, "  read  k=%llu ver=%u len=%u\n", (unsigned long long)tmp2[i].key, tmp2[i].ver, tmp2[i].len);
                std::abort();
            }
        }
#endif
        if (prev != NIL_PAGE) {
            Entry* pe = load(prev);
            if (pe) { LHdr ph; lh_read(pe->b, ph); ph.next = np; lh_write(pe->b, ph); pe->open = 1; mark(*pe, LEAF_HDR, 32); }
        }
        if (next != NIL_PAGE) {
            Entry* ne = load(next);
            if (ne) { LHdr nh; lh_read(ne->b, nh); nh.prev = np; lh_write(ne->b, nh); ne->open = 1; mark(*ne, LEAF_HDR, 32); }
        }
        rc = replace_child(nid, pid, np);
        if (rc != RC::OK) return rc;
        release_page(pid, "settle");
        return RC::OK;
    }

    RC replace_child(u16 nid, u16 oldc, u16 newc) {
#ifdef XDB_ALLOC_TRACE
        std::fprintf(stderr, "[repl] parent=%u %u->%u\n", nid, oldc, newc);
#endif
        set_ev('r', oldc, newc);
        Entry* ce = load(newc);
        if (!ce) return RC::IO;
        u64 nk;
        u16 nnid;
        if (page_kind(ce->b) == PK_LEAF) { LHdr ch; lh_read(ce->b, ch); nk = ch.min_key; ch.nid = nid; lh_write(ce->b, ch); nnid = ch.nid; }
        else { IHdr ch; ih_read(ce->b, ch); ISlot s0; islot_ok(ce->b, 0, s0); nk = s0.key; ch.nid = nid; ih_write(ce->b, ch); nnid = ch.nid; }
        (void)nnid;
        ce->open = 1;
        mark(*ce, LEAF_HDR, 32);
        if (nid == ROOT_PARENT) { root_ = newc; return RC::OK; }
        Entry* pe = load(nid);
        if (!pe) return RC::IO;
        IHdr ih; ih_read(pe->b, ih);
        for (u32 i = 0; i < ih.nslots; ++i) {
            ISlot s;
            if (!islot_ok(pe->b, i, s)) return corrupt_("replace_child slot");
            if (s.child != oldc) continue;
            u64 k = (nk < s.key) ? nk : s.key;     // 只降不升
            islot_write(pe->b, i, k, newc);
            if (i > 0) lower_sep(*pe, i, k);       // 单调性 + 下界
            pe->open = 1;
            mark(*pe, INNER_HDR, 32);
            mark(*pe, islot_off(i), INNER_SLOT);
            if (i == 0) return parent_update(nid);
            return RC::OK;
        }
        return corrupt_("replace_child no-slot");
    }

    // 把槽 i 的分隔键降到 k（只降不升），并向左传播以保持单调
    void lower_sep(Entry& pe, u32 i, u64 k) {
        IHdr ih; ih_read(pe.b, ih);
        u64 cur = k;
        for (u32 j = i + 1; j-- > 0;) {
            ISlot s;
            if (!islot_ok(pe.b, j, s)) return;
            if (s.key <= cur) break;
            islot_write(pe.b, j, cur, s.child);
            mark(pe, islot_off(j), INNER_SLOT);
            if (j == 0) break;
            ISlot pv;
            if (!islot_ok(pe.b, j - 1, pv)) return;
            if (pv.key <= cur) break;
        }
        mark(pe, INNER_HDR, 32);
        pe.open = 1;
#ifdef XDB_ASSERT_SORTED
        check_inner(pe, "lower_sep");
#endif
    }

    RC parent_update(u16 pid) {
        for (int depth = 0; depth < MAX_LEVEL + 2; ++depth) {
            Entry* e = load(pid);
            if (!e) return RC::IO;
            u64 min_key = 0;
            u16 nid = ROOT_PARENT;
            if (page_kind(e->b) == PK_LEAF) {
                LHdr h; lh_read(e->b, h);
                min_key = h.min_key;
                nid = h.nid;
            } else {
                IHdr h; ih_read(e->b, h);
                ISlot s;
                if (!islot_ok(e->b, 0, s)) return corrupt_("parent_update slot0");
                min_key = s.key;
                nid = h.nid;
            }
            if (nid == ROOT_PARENT) return RC::OK;
            Entry* pe = load(nid);
            if (!pe) return RC::IO;
            IHdr ih; ih_read(pe->b, ih);
            bool found = false, up = false;
            for (u32 i = 0; i < ih.nslots; ++i) {
                ISlot s;
                if (!islot_ok(pe->b, i, s)) return corrupt_("parent_update slot");
                if (s.child != pid) continue;
                found = true;
                if (min_key < s.key) {
                    islot_write(pe->b, i, min_key, pid);
                    if (i > 0) lower_sep(*pe, i, min_key);
                    pe->open = 1;
                    mark(*pe, INNER_HDR, 32);
                    mark(*pe, islot_off(i), INNER_SLOT);
                    if (i == 0) { pid = nid; up = true; }
                }
                break;
            }
            if (!found) return corrupt_("parent_update not-found");
            if (!up) return RC::OK;
        }
        return RC::OK;
    }

    // ================================================================ 分裂
    RC split_leaf(u16 pid) {
        Entry* e = load(pid);
        if (!e) return RC::IO;
        LHdr h; lh_read(e->b, h);
        Ent tmp[MAX_SLOTS_E];
        u32 n = gather_view(*e, tmp, MAX_SLOTS_E, open_, true, history_floor());
        static thread_local u8 stage[PAGE];
        u32 sp = 0;
        Ent ents[MAX_SLOTS_E];
        for (u32 i = 0; i < n; ++i) {
            ents[i] = tmp[i];
            if (tmp[i].len) {
                if (sp + tmp[i].len > PAGE) return RC::INVALID;
                std::memcpy(stage + sp, tmp[i].src, tmp[i].len);
                ents[i].src = stage + sp;
                sp += tmp[i].len;
            } else ents[i].src = nullptr;
        }
        {
            // 单键页：无法按键切分，改为「只保留最新版本」的搬迁
            u32 distinct = 0;
            for (u32 i = 0; i < n; ++i)
                if (i == 0 || ents[i].key != ents[i - 1].key) ++distinct;
            if (distinct <= 1) {
                RC rs = settle(pid, true);
                if (rs == RC::OK) return RC::OK;
                return rs == RC::BUSY ? RC::FULL : rs;
            }
        }
        if (n == 0) {                     // 全墓碑：就地清空（保留页与链接）
#ifdef XDB_ALLOC_TRACE
            std::fprintf(stderr, "[split-empty] P%u\n", pid);
#endif
            RC rc0 = build_leaf(pid, ents, 0, h.nid, h.next, h.prev, open_, h.min_key);
            if (rc0 != RC::OK) return rc0;
            return RC::OK;
        }
        u32 cut = n / 2;
        if (cut == 0) cut = 1;
        if (cut >= n) cut = n - 1;
        // 切点必须落在键边界上，避免同一个键被分到两页
        while (cut < n && cut > 0 && ents[cut].key == ents[cut - 1].key) ++cut;
        if (cut >= n) {
            cut = n / 2;
            while (cut > 0 && ents[cut].key == ents[cut - 1].key) --cut;
            if (cut == 0) cut = n - 1;
        }
        u16 np = 0, lp = 0;
        RC rc = alloc_page(np);
        if (rc != RC::OK) return rc;
        rc = alloc_page(lp);
        if (rc != RC::OK) { release_page(np, "split-bail"); return rc; }
        u16 old_next = h.next, old_prev = h.prev, nid = h.nid;
        u64 sep = 0;
        rc = build_leaf(np, ents + cut, n - cut, nid, old_next, lp, open_);
        if (rc != RC::OK) { release_page(np, "split-bail"); release_page(lp, "split-bail"); return rc; }
        {
            Entry* r = load(np);
            LHdr rh; lh_read(r->b, rh);
            sep = rh.min_key;
        }
        rc = build_leaf(lp, ents, cut, nid, np, old_prev, open_);
        if (rc != RC::OK) { release_page(np, "split-bail"); release_page(lp, "split-bail"); return rc; }
#ifdef XDB_SETTLE_CHECK
        {
            Entry* a = load(lp);
            Entry* b2 = load(np);
            Ent t1[MAX_SLOTS_E], t2[MAX_SLOTS_E];
            u32 n1 = gather_view(*a, t1, MAX_SLOTS_E, open_, true, history_floor());
            u32 n2 = gather_view(*b2, t2, MAX_SLOTS_E, open_, true, history_floor());
            if (n1 + n2 != n) {
                std::fprintf(stderr, "[split mismatch] pid=%u n=%u left=%u right=%u cut=%u\n", pid, n, n1, n2, cut);
                for (u32 i = 0; i < n; ++i) std::fprintf(stderr, "  src k=%llu ver=%u len=%u\n", (unsigned long long)ents[i].key, ents[i].ver, ents[i].len);
                for (u32 i = 0; i < n1; ++i) std::fprintf(stderr, "  L k=%llu ver=%u len=%u\n", (unsigned long long)t1[i].key, t1[i].ver, t1[i].len);
                for (u32 i = 0; i < n2; ++i) std::fprintf(stderr, "  R k=%llu ver=%u len=%u\n", (unsigned long long)t2[i].key, t2[i].ver, t2[i].len);
                std::abort();
            }
        }
#endif
        if (old_next != NIL_PAGE) {
            Entry* ne = load(old_next);
            if (ne) { LHdr nh; lh_read(ne->b, nh); nh.prev = np; lh_write(ne->b, nh); ne->open = 1; mark(*ne, LEAF_HDR, 32); }
        }
        if (old_prev != NIL_PAGE) {
            Entry* pe = load(old_prev);
            if (pe) { LHdr ph; lh_read(pe->b, ph); ph.next = lp; lh_write(pe->b, ph); pe->open = 1; mark(*pe, LEAF_HDR, 32); }
        }
        rc = replace_child(nid, pid, lp);
        if (rc != RC::OK) return rc;
        rc = insert_parent(lp, np, sep);
        if (rc != RC::OK) return rc;
        release_page(pid, "split_leaf");
        return RC::OK;
    }

    RC insert_parent(u16 left, u16 right, u64 sep) {
#ifdef XDB_ASSERT_SORTED
        std::fprintf(stderr, "[ins_parent] left=%u right=%u sep=%llu root=%u\n", left, right, (unsigned long long)sep, root_);
#endif
        u16 nid = NIL_PAGE;
        {
            Entry* le = load(left);
            if (!le) return RC::IO;
            if (page_kind(le->b) == PK_LEAF) { LHdr lh; lh_read(le->b, lh); nid = lh.nid; }
            else { IHdr lh; ih_read(le->b, lh); nid = lh.nid; }
        }
        if (nid == ROOT_PARENT) return new_root(left, right, sep);
        Entry* pe = load(nid);
        if (!pe) return RC::IO;
        IHdr ih; ih_read(pe->b, ih);
        if ((u32)ih.nslots + 1 > inner_cap()) return split_inner(nid, left, right, sep);
        Entry* re = load(right);
        if (!re) return RC::IO;
        if (page_kind(re->b) == PK_LEAF) { LHdr rh; lh_read(re->b, rh); rh.nid = nid; lh_write(re->b, rh); mark(*re, LEAF_HDR, 32); }
        else { IHdr rh; ih_read(re->b, rh); rh.nid = nid; ih_write(re->b, rh); mark(*re, INNER_HDR, 32); }
        re->open = 1;
        Entry* pe2 = load(nid);
        if (!pe2) return RC::IO;
        inner_insert_slot(*pe2, right, sep);
        pe2->open = 1;
        return RC::OK;
    }

    u32 inner_cap() const { return (SLOT_CUR0 - INNER_DATA0 - 32) / INNER_SLOT; }

    u64 kids_val(Entry& pe, u32 i) { ISlot s; islot_read(pe.b, i, s); return s.key; }
    u16 child_of(Entry& pe, u32 i) { ISlot s; islot_read(pe.b, i, s); return s.child; }

    void check_inner(Entry& pe, const char* tag) {
        IHdr ih; ih_read(pe.b, ih);
        u64 prev = 0; bool first = true;
        u16 kids[512]; u32 nk = 0;
        for (u32 i = 0; i < ih.nslots && i < 500; ++i) {
            ISlot x;
            if (!islot_ok(pe.b, i, x)) { std::fprintf(stderr, "[chk %s] P%u slot %u bad chk\n", tag, pe.pid, i); std::abort(); }
            if (!first && x.key < prev) {
                std::fprintf(stderr, "[chk %s] P%u unsorted at %u: %llu then %llu\n", tag, pe.pid, i,
                             (unsigned long long)prev, (unsigned long long)x.key);
                std::abort();
            }
            prev = x.key; first = false;
            for (u32 k = 0; k < nk; ++k) if (kids[k] == x.child) {
                std::fprintf(stderr, "[chk %s] P%u duplicate child %u at %u and %u\n", tag, pe.pid, x.child, k, i);
                IHdr h2; ih_read(pe.b, h2);
                for (u32 q = 0; q < h2.nslots && q < 40; ++q) std::fprintf(stderr, "    [%u] %llu -> %u\n", q, (unsigned long long)kids_val(pe, q), child_of(pe, q));
                std::abort();
            }
            kids[nk++] = x.child;
        }
    }

    void inner_insert_slot(Entry& pe, u16 child, u64 key) {
        IHdr ih; ih_read(pe.b, ih);
        ISlot s[512];
        u32 n = ih.nslots;
        if (n > 500) n = 500;
        for (u32 i = 0; i < n; ++i) islot_read(pe.b, i, s[i]);
#ifdef XDB_ASSERT_SORTED
        for (u32 i = 1; i < n; ++i) {
            if (s[i].key < s[i - 1].key) {
                std::fprintf(stderr, "[inner unsorted] pid=%u at %u: %llu then %llu (insert key=%llu)\n",
                             pe.pid, i, (unsigned long long)s[i - 1].key, (unsigned long long)s[i].key,
                             (unsigned long long)key);
                for (u32 j = 0; j < n; ++j)
                    std::fprintf(stderr, "   [%u] %llu -> %u\n", j, (unsigned long long)s[j].key, s[j].child);
                std::abort();
            }
        }
#endif
        u32 at = n;
        for (u32 i = 0; i < n; ++i) if (s[i].key > key) { at = i; break; }
        for (u32 i = n; i > at; --i) s[i] = s[i - 1];
        s[at].key = key;
        s[at].child = child;
        for (u32 i = 0; i < n + 1; ++i) islot_write(pe.b, i, s[i].key, s[i].child);
        wr32(pe.b + 4, n + 1);
        ih.nslots = (u16)(n + 1);
        ih_write(pe.b, ih);
        mark(pe, INNER_HDR, 32);
        mark(pe, INNER_DATA0, INNER_SLOT * (n + 1));
#ifdef XDB_ASSERT_SORTED
        check_inner(pe, "insert_slot");
#endif
    }

    RC new_root(u16 left, u16 right, u64 sep) {
        // 先读孩子信息（load 会换出页面），再写新根，最后才改孩子父指针。
        u64 lk = 0;
        u16 lvl = 1;
        {
            Entry* le = load(left);
            if (!le) return RC::IO;
            if (page_kind(le->b) == PK_LEAF) {
                LHdr lh; lh_read(le->b, lh);
                lk = lh.min_key;
                lvl = 1;
            } else {
                IHdr ch; ih_read(le->b, ch);
                ISlot s0;
                if (!islot_ok(le->b, 0, s0)) return corrupt_("new_root slot0");
                lk = s0.key;
                lvl = (u16)(ch.level + 1);
            }
        }
        if (lk > sep) lk = sep;
        u16 nr = 0;
        RC rc = alloc_page(nr);
        if (rc != RC::OK) return rc;
        Entry* e = make(nr);
        if (!e) return RC::IO;
        page_init(e->b, PK_INNER);
        IHdr ih;
        ih.nid = ROOT_PARENT;
        ih.roll = 0;
        ih.nslots = 2;
        ih.level = lvl;
        ih.ceil = 2;
        ih.sealed_base = 0;
        ih.sealed_cnt = 0;
        ih.b_vers = open_;
        ih_write(e->b, ih);
        wr32(e->b + 4, 2);
        islot_write(e->b, 0, lk, left);
        islot_write(e->b, 1, sep, right);
        Foot f;
        f.vers = open_; f.nslots = 2; f.data_off = 0; f.min_key = 0;
        for (u32 i = 0; i < META_CAP; ++i) f.rolls[i] = 0;
        foot_write_prefix(e->b, f);
        foot_seal(e->b);
        e->open = 1;
        root_ = nr;
        {
            Entry* l2 = load(left);
            if (l2) {
                if (page_kind(l2->b) == PK_LEAF) { LHdr lh; lh_read(l2->b, lh); lh.nid = nr; lh_write(l2->b, lh); mark(*l2, LEAF_HDR, 32); }
                else { IHdr lh; ih_read(l2->b, lh); lh.nid = nr; ih_write(l2->b, lh); mark(*l2, INNER_HDR, 32); }
                l2->open = 1;
            }
            Entry* r2 = load(right);
            if (r2) {
                if (page_kind(r2->b) == PK_LEAF) { LHdr rh; lh_read(r2->b, rh); rh.nid = nr; lh_write(r2->b, rh); mark(*r2, LEAF_HDR, 32); }
                else { IHdr rh; ih_read(r2->b, rh); rh.nid = nr; ih_write(r2->b, rh); mark(*r2, INNER_HDR, 32); }
                r2->open = 1;
            }
        }
        return RC::OK;
    }

    RC split_inner(u16 pid, u16 left, u16 right, u64 sep) {
        Entry* e = load(pid);
        if (!e) return RC::IO;
        IHdr ih; ih_read(e->b, ih);
#ifdef XDB_CORRUPT_TRACE
        std::fprintf(stderr, "[split_inner] pid=%u nslots=%u left=%u right=%u sep=%llu nid=%u\n", pid, ih.nslots, left, right, (unsigned long long)sep, ih.nid);
#endif
        ISlot s[512];
        u32 n = 0;
        for (u32 i = 0; i < ih.nslots && n < 500; ++i) {
            ISlot x;
            if (!islot_ok(e->b, i, x)) return corrupt_("split_inner slot");
            s[n++] = x;
        }
        u32 at = n;
        for (u32 i = 0; i < n; ++i) if (s[i].key > sep) { at = i; break; }
        for (u32 i = n; i > at; --i) s[i] = s[i - 1];
        s[at].key = sep;
        s[at].child = right;
        ++n;
        u32 mid = n / 2;
        if (mid == 0) mid = 1;
        if (mid >= n) mid = n - 1;
        u64 up_key = s[mid].key;
        u16 up_child = s[mid].child;
        u16 npid = 0, lpid = 0;
        RC rc = alloc_page(npid);
        if (rc != RC::OK) return rc;
        rc = alloc_page(lpid);
        if (rc != RC::OK) return rc;
        auto build_inner = [&](u16 target, ISlot* arr, u32 cnt, u16 nid) -> RC {
            Entry* ne = make(target);
            if (!ne) return RC::IO;
            page_init(ne->b, PK_INNER);
            IHdr nh;
            nh.nid = nid;
            nh.roll = 0;
            nh.nslots = (u16)cnt;
            nh.level = ih.level;
            nh.ceil = ih.ceil;
            nh.sealed_base = 0;
            nh.sealed_cnt = 0;
            nh.b_vers = open_;
            ih_write(ne->b, nh);
            wr32(ne->b + 4, cnt);
            for (u32 i = 0; i < cnt; ++i) islot_write(ne->b, i, arr[i].key, arr[i].child);
            Foot f;
            f.vers = open_; f.nslots = cnt; f.data_off = 0; f.min_key = 0;
            for (u32 i = 0; i < META_CAP; ++i) f.rolls[i] = 0;
            foot_write_prefix(ne->b, f);
            foot_seal(ne->b);
            ne->open = 1;
            return RC::OK;
        };
        // 槽是「子页键下界」，因此按 mid 直接对半分，不做中间槽提升
        rc = build_inner(npid, s + mid, n - mid, ih.nid);
        if (rc != RC::OK) return rc;
        rc = build_inner(lpid, s, mid, ih.nid);
        if (rc != RC::OK) return rc;
        // 孩子的父指针
        for (u32 i = 0; i < n; ++i) {
            Entry* ce = load(s[i].child);
            if (!ce) return RC::IO;
            u16 np2 = (i < mid) ? lpid : npid;
            if (page_kind(ce->b) == PK_LEAF) { LHdr ch; lh_read(ce->b, ch); ch.nid = np2; lh_write(ce->b, ch); mark(*ce, LEAF_HDR, 32); }
            else { IHdr ch; ih_read(ce->b, ch); ch.nid = np2; ih_write(ce->b, ch); mark(*ce, INNER_HDR, 32); }
            ce->open = 1;
        }
        rc = replace_child(ih.nid, pid, lpid);
        if (rc != RC::OK) return rc;
        rc = insert_parent(lpid, npid, up_key);
        if (rc != RC::OK) return rc;
        release_page(pid, "split_inner");
        (void)left;
        (void)up_child;
        return RC::OK;
    }

    // ================================================================ 校验
    RC corrupt_(const char* where, u16 pid = 0xFFFF) {
#ifdef XDB_CORRUPT_TRACE
        std::fprintf(stderr, "[corrupt] %s pid=%u\n", where, pid);
        dump_ev();
#endif
        (void)where; (void)pid;
        return RC::CORRUPT;
    }

    void rerr(VerifyReport& r, u16 pid, const char* msg) {
        if (r.errors++ == 0) { r.first_err_page = pid; r.first_err = msg; }
    }

    void verify_node(u16 pid, int expect_level, std::vector<u8>& seen, VerifyReport& r, int depth) {
        if (pid >= dev_->capacity() || depth > MAX_LEVEL + 2) { rerr(r, pid, "page range/depth"); return; }
        if (seen[pid]) return;
        seen[pid] = 1;
        Entry* e = load(pid);
        if (!e) { rerr(r, pid, "load"); return; }
        ++r.pages;
        if (!page_ok(e->b)) { rerr(r, pid, "page hash"); return; }
        u8 k = page_kind(e->b);
        if (k == PK_LEAF) {
            ++r.leaves;
            if (expect_level > 0) rerr(r, pid, "level");
            LHdr h; lh_read(e->b, h);
            if (!foot_ok(e->b)) rerr(r, pid, "footer hash");
            u32 pos[MAX_SLOTS_E];
            u32 n = collect_pubs(*e, pos, MAX_SLOTS_E);
            u64 last_key = 0;
            bool first = true;
            for (u32 i = 0; i < n; ++i) {
                Pub p;
                if (!pub_at(e->b, pos[i], p)) { rerr(r, pid, "slot chk"); continue; }
                ++r.pubs;
                if (p.r_off) {
                    if (p.r_off + VALHDR_SZ > SLOT_CUR0) { rerr(r, pid, "value oob"); continue; }
                    u16 len = rd16(e->b + p.r_off + 2);
                    if (!valhdr_ok(e->b, p.r_off, len)) rerr(r, pid, "value hdr chk");
                }
                if (p.key < h.min_key) rerr(r, pid, "key below min");
                (void)last_key; (void)first;
                if ((p.cred >> 26) >= META_CAP) rerr(r, pid, "region oob");
            }
            Foot f; foot_read(e->b, f);
            for (u32 rg = 0; rg < META_CAP; ++rg) {
                u32 sum = 0;
                for (u32 i = 0; i < n; ++i) {
                    Pub p;
                    if (!pub_at(e->b, pos[i], p)) continue;
                    if ((p.cred >> 26) != rg) continue;
                    u32 hh = mix32((u32)p.key ^ (u32)(p.key >> 32));
                    u32 vh;
                    if (p.r_off) {
                        u16 len = rd16(e->b + p.r_off + 2);
                        vh = atom_hash(e->b + p.r_off + VALHDR_SZ, len, SEED_VALS);
                    } else vh = 0xDEADBEEFu;
                    sum += mix32(hh ^ vh ^ mix32(p.ver));
                }
                if (sum != f.rolls[rg]) rerr(r, pid, "group roll");
            }
            return;
        }
        if (k != PK_INNER) { rerr(r, pid, "bad kind"); return; }
        ++r.inners;
        IHdr h; ih_read(e->b, h);
        if (expect_level >= 0 && h.level != expect_level) rerr(r, pid, "level");
        if (h.nslots == 0) { rerr(r, pid, "inner empty"); return; }
        u64 prev = 0;
        for (u32 i = 0; i < h.nslots; ++i) {
            Entry* e2 = load(pid);              // 递归可能驱逐本页
            if (!e2) { rerr(r, pid, "reload"); return; }
            ISlot s;
            if (!islot_ok(e2->b, i, s)) { rerr(r, pid, "inner slot chk"); return; }
            if (i && s.key < prev) rerr(r, pid, "sep not ascending");
            prev = s.key;
            Entry* ce = load(s.child);
            if (!ce) { rerr(r, pid, "child load"); return; }
            u64 cmin = 0;
            if (page_kind(ce->b) == PK_LEAF) { LHdr ch; lh_read(ce->b, ch); cmin = ch.min_key; }
            else { IHdr ch; ih_read(ce->b, ch); ISlot c0; if (islot_ok(ce->b, 0, c0)) cmin = c0.key; }
            if (s.key > cmin) rerr(r, pid, "sep above child min");
            verify_node(s.child, (int)h.level - 1, seen, r, depth + 1);
        }
    }

    template <class F>
    RC walk_pages(u16 root, F&& fn) {
        std::vector<u8> seen(dev_->capacity(), 0);
        return walk_rec(root, seen, fn, 0);
    }
    template <class F>
    RC walk_rec(u16 pid, std::vector<u8>& seen, F& fn, int depth) {
        if (pid == NIL_PAGE || pid >= dev_->capacity() || seen[pid] || depth > MAX_LEVEL + 2) return RC::OK;
        seen[pid] = 1;
        if (!fn(pid)) return RC::OK;
        Entry* e = load(pid);
        if (!e) return RC::IO;
        if (page_kind(e->b) != PK_INNER) return RC::OK;
        IHdr h; ih_read(e->b, h);
        u16 kids[MAX_SLOTS_E];
        u32 nk = 0;
        for (u32 i = 0; i < h.nslots && nk < MAX_SLOTS_E; ++i) {
            ISlot s;
            if (!islot_ok(e->b, i, s)) break;
            kids[nk++] = s.child;
        }
        for (u32 i = 0; i < nk; ++i) walk_rec(kids[i], seen, fn, depth + 1);
        return RC::OK;
    }

    // 历史保留下界：被快照 pin 的版本必须保留
    Vers history_floor() {
        // 有快照时必须保留全部历史（快照可能引用任意旧版本）
        if (nsnap_ > 0) return 0;
        return (open_ > HORIZON) ? (Vers)(open_ - HORIZON) : 0;
    }

    Vers max_foot_vers() {
        Vers mx = vers_;
        for (u32 i = 0; i < cap_; ++i) {
            if (!ents_[i].valid) continue;
            if (!foot_ok(ents_[i].b)) continue;
            Foot f; foot_read(ents_[i].b, f);
            if (f.vers > mx) mx = f.vers;
        }
        return mx;
    }

    struct RItem { u64 key; u16 ver; u32 voff; u16 len; };

    // 从指定叶集合重建（快照回退）：这些页被 pin，内容只追加不销毁，
    // 因此按版本屏障取值即快照时刻的精确内容。
    RC rebuild_from(Snap& sn, Vers barrier) {
        std::vector<u8> vals;
        std::vector<RItem> items;
        for (u32 li = 0; li < sn.nleaves; ++li) {
            Entry* e = load(sn.leaves[li]);
            if (!e) continue;
            Ent tmp[MAX_SLOTS_E];
            u32 n = gather_view(*e, tmp, MAX_SLOTS_E, barrier, false, 0);
            for (u32 i = 0; i < n; ++i) {
                RItem it;
                it.key = tmp[i].key;
                it.ver = tmp[i].ver;
                it.len = tmp[i].len;
                it.voff = 0;
                if (tmp[i].len) {
                    it.voff = (u32)vals.size();
                    vals.insert(vals.end(), tmp[i].src, tmp[i].src + tmp[i].len);
                }
                items.push_back(it);
            }
        }
        return rebuild_emit(items, vals, barrier);
    }

    // 整树重建（修复用）：从当前叶链收集。
    RC rebuild_all(Vers view) {
        std::vector<u8> vals;
        std::vector<RItem> items;
        u16 pid = leftmost();
        u32 guard = 0;
        while (pid != NIL_PAGE && guard++ < 200000) {
            Entry* e = load(pid);
            if (!e) return RC::IO;
            Ent tmp[MAX_SLOTS_E];
            u32 n = gather_view(*e, tmp, MAX_SLOTS_E, view, false, 0);
            for (u32 i = 0; i < n; ++i) {
                RItem it;
                it.key = tmp[i].key;
                it.ver = tmp[i].ver;
                it.len = tmp[i].len;
                it.voff = 0;
                if (tmp[i].len) {
                    it.voff = (u32)vals.size();
                    vals.insert(vals.end(), tmp[i].src, tmp[i].src + tmp[i].len);
                }
                items.push_back(it);
            }
            LHdr h; lh_read(e->b, h);
            pid = h.next;
        }
        return rebuild_emit(items, vals, view);
    }

    RC rebuild_emit(std::vector<RItem>& items, std::vector<u8>& vals, Vers view) {
        std::sort(items.begin(), items.end(), [](const RItem& a, const RItem& b) {
            return a.key < b.key || (a.key == b.key && a.ver < b.ver);
        });
        // 去重：同键保留全部版本（版本升序）；若最新为墓碑则整组丢弃
        std::vector<RItem> live;
        live.reserve(items.size());
        for (size_t i = 0; i < items.size();) {
            size_t j = i;
            while (j < items.size() && items[j].key == items[i].key) ++j;
            if (items[j - 1].len != 0) {
                u16 prevver = 0xFFFF;
                bool first = true;
                for (size_t k = i; k < j; ++k) {
                    if (!first && items[k].ver == prevver) continue;
                    live.push_back(items[k]);
                    prevver = items[k].ver;
                    first = false;
                }
            }
            i = j;
        }
        std::vector<std::pair<u64, u16>> lvl;
        Ent buf[MAX_SLOTS_E];
        u32 nb = 0, nbytes = 0;
        const u32 BYTES_CAP = SLOT_CUR0 - LEAF_DATA0 - 96;
        u16 last_pid = NIL_PAGE;
        RC rc = RC::OK;
        auto emit = [&](bool) -> RC {
            if (nb == 0 && !lvl.empty()) return RC::OK;
            u16 pid2 = 0;
            RC r = alloc_page(pid2);
            if (r != RC::OK) return r;
            for (u32 i = 0; i < nb; ++i) buf[i].src = buf[i].len ? (vals.data() + buf[i].off) : nullptr;
            Ent one[1];
            one[0] = Ent{0, view, 0, 0, nullptr};
            u64 mk = nb ? buf[0].key : 0;
            r = build_leaf(pid2, nb ? buf : one, nb, ROOT_PARENT, NIL_PAGE, last_pid, view, mk);
            if (r != RC::OK) return r;
            if (last_pid != NIL_PAGE) {
                Entry* lp = load(last_pid);
                if (lp) { LHdr lh; lh_read(lp->b, lh); lh.next = pid2; lh_write(lp->b, lh); lp->open = 1; mark(*lp, LEAF_HDR, 40); }
            }
#ifdef XDB_RB_TRACE
            std::fprintf(stderr, "[emit] P%u n=%u k0=%llu k1=%llu\n", pid2, nb,
                         (unsigned long long)(nb ? buf[0].key : 0), (unsigned long long)(nb ? buf[nb - 1].key : 0));
#endif
            lvl.push_back({mk, pid2});
            last_pid = pid2;
            nb = 0;
            nbytes = 0;
            return RC::OK;
        };
        {
            size_t i = 0;
            while (i < live.size()) {
                size_t j = i;
                u32 gcost = 0;
                while (j < live.size() && live[j].key == live[i].key) {
                    gcost += PUB_SZ + (live[j].len ? (u32)live[j].len + VALHDR_SZ : 0);
                    ++j;
                }
                u32 gcnt = (u32)(j - i);
                // 只在「键组边界」切页，同一键的所有版本必须同页
                if (nb > 0 && (nb + gcnt > MAX_SLOTS_E || nbytes + gcost > BYTES_CAP)) {
                    rc = emit(false);
                    if (rc != RC::OK) return rc;
                }
                for (; i < j; ++i) {
                    Ent en;
                    en.key = live[i].key;
                    en.ver = live[i].ver;
                    en.off = live[i].voff;
                    en.len = live[i].len;
                    en.src = nullptr;
                    buf[nb++] = en;
                    nbytes += PUB_SZ + (live[i].len ? (u32)live[i].len + VALHDR_SZ : 0);
                }
            }
        }
        rc = emit(true);
        if (rc != RC::OK) return rc;
        if (lvl.empty()) return RC::CORRUPT;
        u16 child_level = 0;
        while (lvl.size() > 1) {
            std::vector<std::pair<u64, u16>> up;
            u32 per = 128;
            for (u32 i = 0; i < lvl.size(); i += per) {
                u32 cnt = (u32)std::min<size_t>(per, lvl.size() - i);
                u16 pid2 = 0;
                rc = alloc_page(pid2);
                if (rc != RC::OK) return rc;
                Entry* e = make(pid2);
                if (!e) return RC::IO;
                page_init(e->b, PK_INNER);
                IHdr ih;
                ih.nid = ROOT_PARENT;
                ih.roll = 0;
                ih.nslots = (u16)cnt;
                ih.level = (u16)(child_level + 1);
                ih.ceil = (u16)cnt;
                ih.sealed_base = 0;
                ih.sealed_cnt = 0;
                ih.b_vers = open_;
                ih_write(e->b, ih);
                wr32(e->b + 4, cnt);
                for (u32 j = 0; j < cnt; ++j) islot_write(e->b, j, lvl[i + j].first, lvl[i + j].second);
                Foot f;
                f.vers = view; f.nslots = cnt; f.data_off = 0; f.min_key = 0;
                for (u32 q = 0; q < META_CAP; ++q) f.rolls[q] = 0;
                foot_write_prefix(e->b, f);
                foot_seal(e->b);
                e->open = 1;
                for (u32 j = 0; j < cnt; ++j) {
                    Entry* ce = load(lvl[i + j].second);
                    if (!ce) return RC::IO;
                    if (page_kind(ce->b) == PK_LEAF) { LHdr ch; lh_read(ce->b, ch); ch.nid = pid2; lh_write(ce->b, ch); mark(*ce, LEAF_HDR, 40); }
                    else { IHdr ch; ih_read(ce->b, ch); ch.nid = pid2; ih_write(ce->b, ch); mark(*ce, INNER_HDR, 32); }
                    ce->open = 1;
                }
                up.push_back({lvl[i].first, pid2});
            }
            lvl.swap(up);
            ++child_level;
        }
        root_ = lvl[0].second;
#ifdef XDB_RB_TRACE
        {
            // 重建后自检：叶链链接 + 键序
            {
                u16 d0 = root_;
                for (int d = 0; d < 8; ++d) {
                    Entry* de = load(d0);
                    if (!de) { std::fprintf(stderr, "[rb] descend load fail %u\n", d0); break; }
                    u8 kd = page_kind(de->b);
                    if (kd == PK_LEAF) { LHdr dh; lh_read(de->b, dh); std::fprintf(stderr, "[rb] descend leaf P%u prev=%u next=%u\n", d0, dh.prev, dh.next); break; }
                    IHdr dh; ih_read(de->b, dh);
                    ISlot s0; islot_ok(de->b, 0, s0);
                    std::fprintf(stderr, "[rb] descend P%u level=%u nslots=%u -> P%u\n", d0, dh.level, dh.nslots, s0.child);
                    d0 = s0.child;
                }
            }
            u16 p2 = leftmost(), pv = NIL_PAGE;
            u64 mx = 0; bool hv = false;
            u32 guard = 0;
            while (p2 != NIL_PAGE && guard++ < 100000) {
                if (p2 >= dev_->capacity()) { std::fprintf(stderr, "[rb] bad pid %u\n", p2); break; }
                Entry* e2 = load(p2);
                if (!e2) { std::fprintf(stderr, "[rb] load fail %u\n", p2); break; }
                LHdr h2; lh_read(e2->b, h2);
                if (h2.prev != pv) std::fprintf(stderr, "[rb] prev link P%u prev=%u want=%u\n", p2, h2.prev, pv);
                Ent tt[MAX_SLOTS_E];
                u32 n2 = gather_view(*e2, tt, MAX_SLOTS_E, open_, true);
                if (n2 && hv && tt[0].key <= mx)
                    std::fprintf(stderr, "[rb] order P%u first=%llu prevmax=%llu\n", p2, (unsigned long long)tt[0].key, (unsigned long long)mx);
                if (n2) { mx = tt[n2 - 1].key; hv = true; }
                pv = p2;
                p2 = h2.next;
            }
            std::fprintf(stderr, "[rb] chain ends at P%u, root=%u, leaves=%zu\n", pv, root_, lvl.size());
        }
#endif
        {
            Entry* r = load(root_);
            if (!r) return RC::IO;
            if (page_kind(r->b) == PK_INNER) {
                IHdr ih; ih_read(r->b, ih);
                ih.nid = ROOT_PARENT;
                ih_write(r->b, ih);
                mark(*r, INNER_HDR, 32);
            } else {
                LHdr lh; lh_read(r->b, lh);
                lh.nid = ROOT_PARENT;
                lh_write(r->b, lh);
                mark(*r, LEAF_HDR, 40);
            }
        }
        return RC::OK;
    }

    Entry* e_slot(u16 pid) {
        Entry* e = find(pid);
        return e ? e : &ents_[0];
    }
    void finish_txn() {
        for (u16 pid : olist_) if (pid < oset_.size()) oset_[pid] = 0;
        olist_.clear();
        flog_.clear();
        nalloc_ = 0;
    }

    void release_snap(Snap& s) {
        for (u32 i = 0; i < s.npages; ++i)
            if (s.pages[i] < pin_.size() && pin_[s.pages[i]]) pin_[s.pages[i]]--;
    }

    // ================================================================ 成员
    BlockDev* dev_ = nullptr;
    u8* arena_ = nullptr;
    u32 cap_ = 0;
    Entry ents_[NAV_CAP];
    u32 clock_ = 1;
    u16 root_ = NIL_PAGE;
    u16 root_c_ = NIL_PAGE;      // 上次提交时的根
    Vers vers_ = 0;
    Vers open_ = 1;
    u16 hi_ = 2;
    std::vector<u16> freestack_;
    u32 nfree_ = 0;
    u32 nalloc_ = 0;
    u16 alloc_log_[64];
    std::vector<u8> used_;
    std::vector<u16> pin_;
    std::vector<u8> oset_;
    std::vector<u16> olist_;
    std::vector<u16> flog_;
    Snap snaps_[MAX_SNAP];
    u32 nsnap_ = 0;
    MapRec ring_{};
};

} // namespace xdb
