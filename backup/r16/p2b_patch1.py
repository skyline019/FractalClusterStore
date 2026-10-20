#!/usr/bin/env python3
# P2b pass 1：Run 字段 / 所有权 / 只读保护 / 清单 TRIM / meta 对账 / 溢出链接管规则 / older_may_contain_grp
import sys, re
P = 'include/fcs.h'
s = open(P, encoding='utf-8').read()
orig = s

def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:90])
    s = s.replace(old, new)

# ---- 1. Run 字段 ----
rep("""    u64 live_bytes = 0;
    u32 live_recs = 0;
    // R6-lite：挂载时不算存活账""",
"""    u64 live_bytes = 0;
    u32 live_recs = 0;
    // P2b（第 16 轮）：分片归并的源裁剪。trim ≥ 1 ⇒ 数据页 [1,trim) 已释放（第 0 页留作身份锚，内容是死数据，不再被引用）；
    // first 已推进到裁剪边界键 B（≥ fences[trim]，页 trim 可能是"死活混合页"，键 < B 的那一截靠 first/段 klo 隔离）；
    // nbytes/nrec 是裁后的值。first0/nbytes0/nrec0 = RUN_ADD / RMETA 里的原始值：ensure_meta 用原始值和 RMETA 对账，
    // 清单的 RUN_ADD 也必须写原始值（否则重放时 RMETA 对不上就会退回扫页——扫到已释放的页）。
    u32 trim = 0;
    u64 first0 = 0;
    u32 nbytes0 = 0, nrec0 = 0;
    // 归并片进行中：页号 ∈ [1, take_lim) 的记录才允许"零读接管"溢出链（这些页在本片封口时释放）；第 0 页和 ≥ take_lim 的页
    // 本片不释放 ⇒ 链仍归源 run，记录按物化拷贝。NIL = 不限（源 run 本次整条销毁）。
    u32 take_lim = NIL;
    // R6-lite：挂载时不算存活账""")

# ---- 2. man_ok ----
rep("""        if (m.op < 1 || m.op > 6) return false;
        if (m.op == M_RANGE) return m.key < m.last && m.last <= IDX_BASE;""",
"""        if (m.op < 1 || m.op > M_RUN_TRIM) return false;          // v2：7 = RUN_TRIM；8（RUN_HOLE）只预留号，本版不接受
        if (m.op == M_RANGE) return m.key < m.last && m.last <= IDX_BASE;
        if (m.op == M_RUN_TRIM) return m.page0 >= 2 && m.seq != 0;""")

# ---- 3. apply_man：RUN_ADD 记原始值；RUN_TRIM ----
rep("""            r->first = m.key; r->last = m.last;
            r->ver = m.ver; r->nbytes = m.nbytes;
            r->seq = m.seq; r->lvl = std::min<u32>(m.lvl, LVL_MAX);   // 坑 89""",
"""            r->first = m.key; r->last = m.last;
            r->ver = m.ver; r->nbytes = m.nbytes;
            r->first0 = m.key; r->nbytes0 = m.nbytes;                   // P2b：原始值（TRIM 之后 first/nbytes 会变）
            r->seq = m.seq; r->lvl = std::min<u32>(m.lvl, LVL_MAX);   // 坑 89""")
rep("""        } else if (m.op == M_RUN_DEL) {
            auto it = runs_.find(m.page0);
            if (it != runs_.end()) { delete it->second; runs_.erase(it); }""",
"""        } else if (m.op == M_RUN_TRIM) {
            // P2b：源前缀裁剪。只对"同 page0 且 seq 相同"的 run 生效（页号会复用；TRIM 永远晚于它的 ADD、早于它的 DEL，
            // 快照重写时紧跟在 ADD 之后）。npages = 新的起始页偏移（0 = 没释放页、只推进 first），key = 裁后 first，nbytes = 裁后值。
            auto it = runs_.find(m.page0);
            if (it == runs_.end() || it->second->seq != m.seq || m.npages >= it->second->npages) { ++bad_man_; return; }
            RunPtr r = it->second;
            if (m.npages > r->trim) r->trim = m.npages;
            if (m.key > r->first) r->first = m.key;
            r->nbytes = m.nbytes;
            if (r->live_bytes > r->nbytes) r->live_bytes = r->nbytes;
            if (r->meta_pending.load(std::memory_order_relaxed) == 0) apply_trim_meta(r);   // 重放时 meta 还没装：留给 ensure_meta
        } else if (m.op == M_RUN_DEL) {
            auto it = runs_.find(m.page0);
            if (it != runs_.end()) { delete it->second; runs_.erase(it); }""")

# ---- 4. run_ent / trim_ent ----
rep("""    ManEnt run_ent(RunPtr r, u8 op) {
        ManEnt m;
        m.op = op;
        m.key = r->first; m.last = r->last;
        m.page0 = r->page0; m.npages = r->npages;
        m.ver = r->ver; m.nbytes = r->nbytes; m.seq = r->seq; m.lvl = (u8)r->lvl;
        return m;
    }""",
"""    ManEnt run_ent(RunPtr r, u8 op) {
        ManEnt m;
        m.op = op;
        // P2b：RUN_ADD 写**原始** first/nbytes（重放时 ensure_meta 拿它们和 RMETA 对账）；裁后的值由紧随其后的 RUN_TRIM 项携带
        m.key = op == M_RUN_ADD ? r->first0 : r->first; m.last = r->last;
        m.page0 = r->page0; m.npages = r->npages;
        m.ver = r->ver; m.nbytes = op == M_RUN_ADD ? r->nbytes0 : r->nbytes; m.seq = r->seq; m.lvl = (u8)r->lvl;
        return m;
    }
    ManEnt trim_ent(RunPtr r) { ManEnt m = run_ent(r, M_RUN_TRIM); m.npages = r->trim; return m; }
    bool run_trimmed(const Run* r) const { return r->trim != 0 || r->first != r->first0; }""")

# ---- 5. write_man_snapshot：ADD 后补 TRIM ----
rep("""        for (auto& kv : runs_) {
            all.push_back(run_ent(kv.second, M_RUN_ADD));
        }
        for (auto& kv : ranges_) {""",
"""        for (auto& kv : runs_) {
            all.push_back(run_ent(kv.second, M_RUN_ADD));
            if (run_trimmed(kv.second)) all.push_back(trim_ent(kv.second));   // P2b：裁过的源 run，ADD 之后紧跟一条 TRIM
        }
        for (auto& kv : ranges_) {""")

# ---- 6. finish_run：原始值 ----
rep("""        r->page0 = p0;
        r->first = w.first; r->last = w.last;
        r->ver = w.ver; r->nbytes = w.nbytes; r->nrec = w.nrec; r->novfl = w.novfl;""",
"""        r->page0 = p0;
        r->first = w.first; r->last = w.last;
        r->ver = w.ver; r->nbytes = w.nbytes; r->nrec = w.nrec; r->novfl = w.novfl;
        r->first0 = r->first; r->nbytes0 = r->nbytes; r->nrec0 = r->nrec;   // P2b：新 run 没裁过""")

# ---- 7. ensure_meta：用原始值对账 + apply_trim_meta ----
rep("""        if (r->mpage) {
            ManEnt m;
            m.page0 = r->page0; m.npages = r->npages;
            m.key = r->first; m.last = r->last; m.nbytes = r->nbytes;
            if (load_rmeta(r, r->mpage, r->npages, m)) {
                r->mnpg = rmeta_pages(rmeta_size(r));
                meta_hits_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        r->mnpg = 0;
        scan_meta(r);
    }""",
"""        if (r->mpage) {
            ManEnt m;
            m.page0 = r->page0; m.npages = r->npages;
            m.key = r->first0; m.last = r->last; m.nbytes = r->nbytes0;   // P2b：RMETA 记的是原始 first/nbytes（裁剪不改 meta 页）
            if (load_rmeta(r, r->mpage, r->npages, m)) {
                r->mnpg = rmeta_pages(rmeta_size(r));
                meta_hits_.fetch_add(1, std::memory_order_relaxed);
                apply_trim_meta(r);
                return;
            }
        }
        r->mnpg = 0;
        scan_meta(r);
        apply_trim_meta(r);
    }
    // P2b：裁后的 nbytes/nrec = 原始值 − 前缀 [0,trim) 的页字节/条数（第 0 页虽然还在，内容也算死）。
    // 栅栏 / 前缀和保留全部页（page_of、attach_run 的栅栏指针要求单调完整），只是 [1,trim) 永远不会被读。
    void apply_trim_meta(Run* r) {
        if (!r || !r->trim) return;
        if (r->pcum.size() <= r->trim || r->pncum.size() <= r->trim) return;
        const u64 pb = r->pcum[r->trim]; const u32 pn = r->pncum[r->trim];
        r->nbytes = (u32)((u64)r->nbytes0 > pb ? (u64)r->nbytes0 - pb : 0);
        r->nrec = r->nrec0 > pn ? r->nrec0 - pn : 0;
        if (r->live_bytes > r->nbytes) r->live_bytes = r->nbytes;
    }""")

# load_rmeta：记 nrec0
rep("""        r->bloom.nk = nrec;
        r->nrec = nrec;
        // novfl 没存进 meta（省 4 B/run）。""",
"""        r->bloom.nk = nrec;
        r->nrec = nrec; r->nrec0 = nrec;
        // novfl 没存进 meta（省 4 B/run）。""")

# ---- 8. scan_meta：跳过已释放的 [1,trim) ----
rep("""        r->pnrec.assign(r->npages, 0);
        for (u32 i = 0; i < r->npages; ++i) {
            const u8* p = page(r->page0 + i);
            if (!p) continue;
            std::memcpy(&r->fences[i], p + 16, 8);
            u16 used = rd16(p + 34);""",
"""        r->pnrec.assign(r->npages, 0);
        for (u32 i = 0; i < r->npages; ++i) {
            if (i && i < r->trim) { r->fences[i] = r->fences[i - 1]; continue; }   // P2b：已释放的前缀页不读；栅栏填前一页的（保持单调）
            const u8* p = page(r->page0 + i);
            if (!p) continue;
            std::memcpy(&r->fences[i], p + 16, 8);
            u16 used = rd16(p + 34);""")
rep("""                    ++off2;
                }
            }
        }
        rebuild_pcum(r);
    }""" if False else """        rebuild_pcum(r);
    }

    // 把一个 run 的派生数组准备好""",
"""        r->nrec0 = r->nrec;                       // P2b：扫出来的就是"原始值"（裁过的 run 扫描只含第 0 页 + 存活区，apply_trim_meta 再扣第 0 页）
        rebuild_pcum(r);
    }

    // 把一个 run 的派生数组准备好""")

# ---- 9. ensure_ovfl_loc：附件清单按 trim 过滤；扫描跳过 [1,trim) ----
rep("""            if (load_ovfl_aux(r->aux_pg, r->aux_npg, r->page0, where, nullptr)) {
                for (auto& e : where) r->ovfl_loc.push_back(e);
                return;
            }
        }
        // 附件不可用：老老实实扫一遍数据页（慢，但只发生在"老库/附件写失败"时）
        for (u32 i = 0; i < r->npages; ++i) {
            const u8* p = page(r->page0 + i);""",
"""            if (load_ovfl_aux(r->aux_pg, r->aux_npg, r->page0, where, nullptr)) {
                // P2b：裁掉的前缀页 [1,trim) 上的链要么被片产物接管、要么在封口时已释放 ⇒ 都不再归本 run（附件页本身不改写）。
                // 第 0 页的链从不被接管（take_lim 规则）⇒ 仍归本 run，整条 run 死时一起还。
                for (auto& e : where)
                    if (!(r->trim && e.first > r->page0 && e.first < r->page0 + r->trim)) r->ovfl_loc.push_back(e);
                return;
            }
        }
        // 附件不可用：老老实实扫一遍数据页（慢，但只发生在"老库/附件写失败"时）
        for (u32 i = 0; i < r->npages; ++i) {
            if (i && i < r->trim) continue;                       // P2b：已释放的前缀页
            const u8* p = page(r->page0 + i);""")

# ---- 10. 所有权：for_owned / free_run_pages ----
rep("""    static u32 run_block(const Run* r) { return r->npages + r->mnpg + r->aux_npg; }
    u32 live_top() {""",
"""    static u32 run_block(const Run* r) { return r->npages + r->mnpg + r->aux_npg; }
    // P2b：run 块里**归它所有**的页：{page0} ∪ [page0+trim, page0+run_block)（trim == 0 ⇒ 整块一段）。
    // 释放 / 重建空闲表 / verify 认领都走这里，别再用 free_pages(page0, run_block) 整块释放（裁过的前缀已经是别人的了）。
    template <class F> static void for_owned(const Run* r, F&& f) {
        const u32 blk = run_block(r);
        if (!r->trim || r->trim >= blk) { f(r->page0, blk); return; }
        f(r->page0, 1u);
        f(r->page0 + r->trim, blk - r->trim);
    }
    void free_run_pages(RunPtr r, bool safe) { for_owned(r, [&](u32 p, u32 n) { free_pages(p, n, safe); }); }
    u32 live_top() {""")
rep("""            const bool safe = run_page_safe(dead);      // 走到这里必有 ver > vmax（或没有快照）
            free_ovfl_of(dead, safe);
            free_pages(dead->page0, run_block(dead), safe);""",
"""            const bool safe = run_page_safe(dead);      // 走到这里必有 ver > vmax（或没有快照）
            free_ovfl_of(dead, safe);
            free_run_pages(dead, safe);""")
rep("""            ensure_aux(kv.second);
            used.push_back({kv.second->page0, run_block(kv.second)});
            // 溢出链 / 附件页也是活数据""",
"""            ensure_aux(kv.second);
            for_owned(kv.second, [&](u32 p, u32 n) { used.push_back({p, n}); });   // P2b：裁过的前缀不是它的
            // 溢出链 / 附件页也是活数据""")
rep("""            ensure_aux(r);
            claim(r->page0, run_block(r), 1);
            std::vector<std::pair<u32, u32>> ex;""",
"""            ensure_aux(r);
            for_owned(r, [&](u32 p, u32 n) { claim(p, n, 1); });
            std::vector<std::pair<u32, u32>> ex;""")
# merge_run_set 的源释放 / 作业落地 / @6837 的释放：统一替换剩余的 free_pages(r->page0, run_block(r), safe)
n_left = s.count("free_pages(r->page0, run_block(r), safe);")
assert n_left == 3, n_left
s = s.replace("free_pages(r->page0, run_block(r), safe);", "free_run_pages(r, safe);")

# ---- 11. verify 第 1 步：跳过 [1,trim) ----
rep("""            u64 prev = 0; bool first = true;
            for (u32 i = 0; i < r->npages; ++i) {
                const u8* p = page(r->page0 + i);
                if (!p) { verr(rep, "run page"); break; }
                if (rd32(p) != PK_DATA) { verr(rep, "page kind"); break; }""",
"""            u64 prev = 0; bool first = true;
            if (r->trim >= r->npages) verr(rep, "trim range");
            for (u32 i = 0; i < r->npages; ++i) {
                if (i && i < r->trim) continue;                // P2b：已释放的前缀页（第 0 页仍完整：CRC / 栅栏 / 链照查）
                const u8* p = page(r->page0 + i);
                if (!p) { verr(rep, "run page"); break; }
                if (rd32(p) != PK_DATA) { verr(rep, "page kind"); break; }""")

# ---- 12. 只读保护：probe / page_range / attach_run / Cur::load ----
rep("""        if (p0 >= p1) return RC::NOTFOUND;
        if (r->meta_pending) ensure_meta(r);
        if (key < r->fences[p0]) return RC::NOTFOUND;""",
"""        if (key < r->first) return RC::NOTFOUND;          // P2b：裁剪边界之下的键（混合页上的死副本）对读不存在
        if (p0 < r->trim) p0 = r->trim;                   // 已释放的前缀页不读
        if (p0 >= p1) return RC::NOTFOUND;
        if (r->meta_pending) ensure_meta(r);
        if (key < r->fences[p0]) return RC::NOTFOUND;""")
rep("""        if (i0 >= r->npages) { p0 = p1 = r->npages; return; }
        p0 = i0;
        p1 = std::min<u32>(i1 + 1, r->npages);
        if (p1 <= p0) p1 = std::min<u32>(p0 + 1, r->npages);
    }""",
"""        if (i0 >= r->npages) { p0 = p1 = r->npages; return; }
        p0 = std::max(i0, r->trim);                        // P2b：不越过裁剪边界
        p1 = std::min<u32>(i1 + 1, r->npages);
        if (p1 <= p0) p1 = std::min<u32>(p0 + 1, r->npages);
    }""")
rep("""            u32 p0 = pi, p1 = std::min<u32>(pj + 1, np);
            if (p1 <= p0) p1 = std::min<u32>(p0 + 1, np);
            pi = pj;""",
"""            u32 p0 = std::max(pi, r->trim), p1 = std::min<u32>(pj + 1, np);   // P2b：段不进已释放的前缀页
            if (p1 <= p0) p1 = std::min<u32>(p0 + 1, np);
            pi = pj;""")
rep("""        bool load(Store* s, u64 from = 0) {
            if (!r || p0 >= p1) { done = true; return false; }
            pi = p0; off = DHDR;""",
"""        bool load(Store* s, u64 from = 0) {
            if (r && p0 < r->trim) p0 = r->trim;             // P2b：已释放的前缀页不读
            if (!r || p0 >= p1) { done = true; return false; }
            pi = p0; off = DHDR;""")

# ---- 13. merge_run_set 的游标：从 trim 页、裁后 first 起 ----
rep("""            c.r = r; c.p0 = 0; c.p1 = r->npages; c.klo = 0; c.khi = ~0ull;
            c.load(this);
        }""",
"""            c.r = r; c.p0 = r->trim; c.p1 = r->npages; c.klo = r->first; c.khi = ~0ull;   // P2b：裁过的源：混合页上 < first 的死副本不归并
            c.load(this);
        }""")

# ---- 14. 溢出链接管：页号规则 ----
rep("""                } else if (take_ok(cs[best].r, dsc)) {
                    bv = nullptr; blen = plen; took = true;   // 接管：fn 用 dsc，不碰 bv""",
"""                } else if (take_ok(cs[best].r, dsc) && take_pg_ok(cs[best])) {
                    bv = nullptr; blen = plen; took = true;   // 接管：fn 用 dsc，不碰 bv""")
rep("""    bool take_ok(RunPtr src, const u8* dsc) const {
        if (!ovfl_take_) return false;""",
"""    // P2b：分片作业里只有本片封口时会释放的页（[1, take_lim)）上的记录才接管链；第 0 页 / 边界混合页的链留给源 run
    //（源整条死时一起还），否则重开后按附件页重建的链清单会把已被片产物接管的链当自己的还掉。
    static bool take_pg_ok(const Cur& c) { return c.r->take_lim == NIL || (c.pi >= 1 && c.pi < c.r->take_lim); }
    bool take_ok(RunPtr src, const u8* dsc) const {
        if (!ovfl_take_) return false;""")

# ---- 15. older_may_contain_grp ----
rep("""    bool older_may_contain(u64 k, SegKey me) const {
        const u64 hk = khash(k);
        for (auto& kv : runs_) {
            RunPtr r = kv.second;
            if (!r->npages || seg_key(r->seq, r->page0) >= me) continue;
            if (k < r->first || k > r->last) continue;
            if (r->bloom.may(hk)) return true;
        }
        return false;
    }""",
"""    bool older_may_contain(u64 k, SegKey me) const {
        const u64 hk = khash(k);
        for (auto& kv : runs_) {
            RunPtr r = kv.second;
            if (!r->npages || seg_key(r->seq, r->page0) >= me) continue;
            if (k < r->first || k > r->last) continue;
            if (r->bloom.may(hk)) return true;
        }
        return false;
    }
    // P2b：层合并版——"组外、且不比组更新（seq ≤ maxseq）"的 run 里是否可能有 k 的物理副本。以前用"全序在组内最老 run 之前"，
    // 隐含前提是 seq ∈ [组最小, 组最大] 的 run 都在组内（坑 91 的连续前缀规则）；分片之后同 seq 的片产物挂在上一层、不在组里
    //（作业作废后留下：源 A/B 裁过、片 P(seq=B.seq) 在 lvl+1），新 L0 的墓碑在下一次合并时按老规则看不到 P ⇒ 墓碑被丢、P 里的老值复活。
    // 组内 run（is_merge_src）跳过：它们的副本正在这次归并里被墓碑压掉。
    bool older_may_contain_grp(u64 k, u64 maxseq) const {
        const u64 hk = khash(k);
        for (auto& kv : runs_) {
            RunPtr r = kv.second;
            if (!r->npages || r->seq > maxseq || is_merge_src(r)) continue;
            if (k < r->first || k > r->last) continue;
            if (r->bloom.may(hk)) return true;
        }
        return false;
    }""")
rep("""            if (l == 0 && !dsc && !older_may_contain(k, minkey)) {   // 坑 94：下面没有更老的副本 ⇒ 墓碑不再往下带""",
"""            if (l == 0 && !dsc && !older_may_contain_grp(k, maxseq)) {   // 坑 94：下面没有更老的副本 ⇒ 墓碑不再往下带（P2b：组外同 seq 的片也算"下面"）""")
rep("""                    if (l == 0 && !dsc && !older_may_contain(k, minkey)) {                                      // 坑 94：墓碑不往下带""",
"""                    if (l == 0 && !dsc && !older_may_contain_grp(k, maxseq)) {                                  // 坑 94：墓碑不往下带（P2b 版，见 older_may_contain_grp）""")

# ---- 16. hw_reclaim_pass：有裁过的 run 时不搬（块内有洞，窗规划按整块算会错） ----
rep("""        if (job_ || !hw_reclaim_pct_ || !dev_) return false;
        const u64 live = live_page_count();""",
"""        if (job_ || !hw_reclaim_pct_ || !dev_) return false;
        for (auto& kv : runs_) if (kv.second->trim) return false;   // P2b：裁过的源 run 块内有洞（作业作废后残留），二步搬家按整块规划会错；等层合并把它们并掉
        const u64 live = live_page_count();""")

# ---- 17. 枚举注释 ----
rep("""M_RUN_TRIM = 7,  // v2（P2b）：源 run 前缀释放：page0=rid npages=新的起始页偏移（run 内，≥ 1；第 0 页永远留作身份""",
"""M_RUN_TRIM = 7,  // v2（P2b）：源 run 前缀释放：page0=rid npages=新的起始页偏移（run 内，0 = 只推进 first；第 0 页永远留作身份""")

assert s != orig
open(P, 'w', encoding='utf-8').write(s)
print("pass1 ok", len(orig), "->", len(s))
