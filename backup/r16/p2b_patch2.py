#!/usr/bin/env python3
# P2b pass 2：MergeJob 分片（job_land）、start_job 分片大小/预留、level_pass 家族、API/计数器
P = 'include/fcs.h'
s = open(P, encoding='utf-8').read()
orig = s

def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

def splice(a_anchor, b_anchor, new):
    """把 [a_anchor 起, b_anchor 起) 替换成 new（b_anchor 保留）"""
    global s
    assert s.count(a_anchor) == 1, a_anchor[:80]
    assert s.count(b_anchor) == 1, b_anchor[:80]
    a = s.index(a_anchor); b = s.index(b_anchor)
    assert a < b
    s = s[:a] + new + s[b:]

# ---- 1. API / 成员 ----
rep("""    void set_merge_budget_pages(u32 n) { merge_budget_ = n; }
""",
"""    void set_merge_budget_pages(u32 n) { merge_budget_ = n; }
    // P2b（第 16 轮）：分片归并。分步作业的产物每攒够 n 页就独立成一条 run 落地（同 seq、同层 ⇒ 与后续片是一个"家族"），
    // 源 run 同时裁掉已归并的前缀（RUN_TRIM：页 [1, B 所在页) 释放，第 0 页留作身份锚）⇒ 合并时刻的峰值从"源 + 整个产物"
    // 降到"源 + 约两片"。0 = 不分片（老行为：一个产物）；不调用 = 自适应 max(FCS_PART_MIN_PAGES, 产物估计/64)。
    // 只对分步作业生效（mode 0 不开作业 ⇒ 不分片，坑 101）。
    void set_merge_part_pages(u32 n) { part_pages_ = n; part_auto_ = false; }
    void set_merge_part_auto() { part_auto_ = true; }
    u64  merge_parts() const { return merge_parts_; }     // 中途封口（片落地）次数
    u64  run_trims() const { return run_trims_; }         // 写出的 RUN_TRIM 项数
""")
rep("""    u32 merge_budget_ = FCS_MERGE_BUDGET_DEFAULT;   // 每片源页预算（页）；0 = 不切片
""",
"""    u32 merge_budget_ = FCS_MERGE_BUDGET_DEFAULT;   // 每片源页预算（页）；0 = 不切片
#ifndef FCS_PART_MIN_PAGES
#define FCS_PART_MIN_PAGES 1024            // P2b：分片归并的最小片（页，4 MB）；自适应 = max(这个, 产物估计/64)
#endif
    u32 part_pages_ = 0;                   // set_merge_part_pages 固定值（part_auto_ = false 时生效；0 = 不分片）
    bool part_auto_ = true;
    u64 merge_parts_ = 0, run_trims_ = 0;
""")

# ---- 2. MergeJob 字段 ----
rep("""        u64 lb = 0, moved = 0, dropped_tombs = 0;
        u32 slices = 0;
    };
    std::unique_ptr<MergeJob> job_;""",
"""        u64 lb = 0, moved = 0, dropped_tombs = 0;
        u32 slices = 0;
        // P2b：分片。part = 片大小（页，0 = 不分片）；src_vis[i] = 当前片里源 i 的"以前可见、现在不可见"的赢家字节
        //（封口时从源 live_bytes 扣——估计量，只影响脏度判断）
        u32 part = 0;
        std::vector<u64> src_vis;
    };
    std::unique_ptr<MergeJob> job_;""")

# ---- 3. start_job：分片大小 + 预留上限 ----
rep("""        j.newlvl = newlvl;
        j.next_lo = j.kmin;
        j.w.reset(new RunWriter(j.out_ver, exp));
        j.w->st = this;
        j.w->want_kidx = keyidx_;
        begin_stream(*j.w, set, cap_hint);         // R12：作业跨片 / 跨提交点，预留区间随作业存活（abandon_job 归还）
    }""",
"""        j.newlvl = newlvl;
        j.next_lo = j.kmin;
        j.src_vis.assign(j.src.size(), 0);
        // P2b：片大小。自适应 = max(FCS_PART_MIN_PAGES, 产物估计/64)（产物估计 = Σ 源存活字节折页，与 stream_cap_est 同口径）
        if (part_auto_) {
            u64 lbp = 0;
            for (RunPtr r : set) if (r) lbp += (std::min<u64>(r->live_bytes, r->nbytes) + (PAGE - DHDR) - 1) / (PAGE - DHDR);
            j.part = (u32)std::max<u64>(FCS_PART_MIN_PAGES, lbp / 64);
        } else j.part = part_pages_;
        j.w.reset(new RunWriter(j.out_ver, exp));
        j.w->st = this;
        j.w->want_kidx = keyidx_;
        // 分片时预留只按一片的量（整个产物的预留正是 2× 峰值的来源）；调用方给了 cap_hint（单源搬家）就照它的
        if (!cap_hint && j.part) { const u64 est = stream_cap_est(set); const u64 pc = (u64)j.part + j.part / 8 + 8; if (est > pc) cap_hint = pc; }
        begin_stream(*j.w, set, cap_hint);         // R12：作业跨片 / 跨提交点，预留区间随作业存活（abandon_job 归还）
    }""")

# ---- 4. merge_job_step：片内改动 + 封口 ----
rep("""        if (!j.done) {
            // ---- 2. 定片：从 next_lo 所在单元起，按键序累计源段页数到 ≥ 预算 ----""",
"""        if (j.src_vis.size() != src.size()) j.src_vis.assign(src.size(), 0);
        if (!j.done) {
            // ---- 2. 定片：从 next_lo 所在单元起，按键序累计源段页数到 ≥ 预算 ----""")
rep("""                auto src_index = [&](RunPtr r) -> size_t { for (size_t i = 0; i < src.size(); ++i) if (src[i] == r) return i; return 64; };
                take_set_.clear();
                for (RunPtr r : src) take_set_.insert(r->page0);   // 产物落地时源 run 整条销毁 ⇒ 链可以接管""",
"""                auto src_index = [&](RunPtr r) -> size_t { for (size_t i = 0; i < src.size(); ++i) if (src[i] == r) return i; return 64; };
                take_set_.clear();
                for (RunPtr r : src) take_set_.insert(r->page0);   // 产物落地时源 run 整条销毁 ⇒ 链可以接管
                // P2b：分片作业里只有"本片封口时会释放的页"（[1, hi_s 所在页)）上的记录才接管链（take_pg_ok）；最后一片不限
                struct TakeGuard { std::vector<RunPtr>& v; bool armed = true; ~TakeGuard() { if (armed) for (RunPtr r : v) r->take_lim = NIL; } } take_guard{src};
                if (j.part && hi_s != ~0ull) for (RunPtr r : src) r->take_lim = page_of(r, hi_s);""")
rep("""                    bool vis = false;                      // 源副本以前对读可见（来源 run 在本单元有段）
                    if (a && sr) { const size_t si = src_index(sr); vis = si < 64 && ((a->smask >> si) & 1ull); }
                    if (l == 0 && !dsc && !older_may_contain_grp(k, maxseq)) {                                  // 坑 94：墓碑不往下带（P2b 版，见 older_may_contain_grp）
                        if (a && vis && !(newer_may_contain(k, maxseq, 0) && newer_has(k, maxseq, 0xFFFFFFFFu))) j.acc[a->ai].vis_old += osz;   // 以前可见、现在物理消失
                        ++j.dropped_tombs; return true;
                    }""",
"""                    bool vis = false;                      // 源副本以前对读可见（来源 run 在本单元有段）
                    size_t si = 64;
                    if (a && sr) { si = src_index(sr); vis = si < 64 && ((a->smask >> si) & 1ull); }
                    if (l == 0 && !dsc && !older_may_contain_grp(k, maxseq)) {                                  // 坑 94：墓碑不往下带（P2b 版，见 older_may_contain_grp）
                        if (a && vis && !(newer_may_contain(k, maxseq, 0) && newer_has(k, maxseq, 0xFFFFFFFFu))) {   // 以前可见、现在物理消失
                            j.acc[a->ai].vis_old += osz;
                            if (si < j.src_vis.size()) j.src_vis[si] += osz;
                        }
                        ++j.dropped_tombs; return true;
                    }""")
rep("""                    if (a) { MergeJob::JAcc& ja = j.acc[a->ai]; ++ja.em; if (vis) ja.vis_old += osz; }
                    return true;
                });
                take_set_.clear();
                if (!ok) { abandon_job(); return RC::OK; }      // 空间不够：尽力而为（与 merge_run_set 的 NOSPACE 同义）
                ++j.slices;
                ++maint_slices_;
                if (cut != ~0ull) {                             // 让了路：cut 之后的单元格没归并，退掉它们的作业账（钩子不能对没归并的单元生效）
                    while (!j.acc.empty() && j.acc.back().c->lo >= cut) { j.acc.back().c->jacc = 0; j.acc.back().c->jdirty = false; j.acc.pop_back(); }
                    ++maint_yields_;
                    j.next_lo = cut;
                    return RC::MORE;
                }
                j.next_lo = hi_s;
                if (reached_end || hi_s == ~0ull) j.done = true;
                if (!j.done) return RC::MORE;
            }
        }""",
"""                    if (a) { MergeJob::JAcc& ja = j.acc[a->ai]; ++ja.em; if (vis) { ja.vis_old += osz; if (si < j.src_vis.size()) j.src_vis[si] += osz; } }
                    return true;
                });
                take_set_.clear();
                for (RunPtr r : src) r->take_lim = NIL;
                take_guard.armed = false;                       // 下面 job_land 可能删掉源 Run 对象，src 里的指针随之失效
                if (!ok) { abandon_job(); return RC::OK; }      // 空间不够：尽力而为（与 merge_run_set 的 NOSPACE 同义）
                ++j.slices;
                ++maint_slices_;
                if (cut != ~0ull) {                             // 让了路：cut 之后的单元格没归并，退掉它们的作业账（钩子不能对没归并的单元生效）
                    while (!j.acc.empty() && j.acc.back().c->lo >= cut) { j.acc.back().c->jacc = 0; j.acc.back().c->jdirty = false; j.acc.pop_back(); }
                    ++maint_yields_;
                    j.next_lo = cut;
                } else {
                    j.next_lo = hi_s;
                    if (reached_end || hi_s == ~0ull) j.done = true;
                }
                // P2b：分片封口——产物攒够一片且作业还没完 ⇒ 这一片独立成 run 落地、源裁前缀，然后换一个新的 RunWriter 接着归并
                if (!j.done && j.part && j.w && j.w->total_pages() >= j.part) {
                    RC pr = job_land(delta, false);
                    if (pr != RC::OK) return pr;
                    if (!job_) return RC::OK;                   // 封口失败 ⇒ 作业已作废（已落的片 / 已裁的源自洽）
                }
                if (!j.done) return RC::MORE;
            }
        }
        // ---- 4. 落地：最后一片 ----
        return job_land(delta, true);
    }

    // P2b：作业落地。final = true：最后一片，源整条退休（老行为）；false：中途封口——这一片独立成 run（lvl = newlvl、seq = maxseq，
    // 与后续片 / 最终产物同 seq ⇒ 读者按 (seq, rid) 排序时它们键域两两不交，谁先谁后无所谓），源 run 只摘 [·, B) 的段并裁掉
    // 已归并前缀（B = j.next_lo，一定是单元边界）：页 [max(1,trim), B 所在页) 释放，first → B；B 所在页若是死活混合页就留下，
    // 其上 < B 的死副本靠 first / 段 klo 隔离（probe 对 key < first 直接 NOTFOUND）。源 last < B ⇒ 整条退休。
    // 账：摘段即精确扣 live_recs；live_bytes 扣本片累计的"以前可见"字节（估计量）。单元增量账与老路一致，封口后清零重来。
    RC job_land(std::vector<ManEnt>& delta, bool final) {
        if (!job_) return RC::OK;
        MergeJob& j = *job_;
        std::vector<RunPtr> src;
        for (auto& e : j.src) { auto it = runs_.find(e.first); if (it != runs_.end() && it->second->seq == e.second) src.push_back(it->second); }
        const u64 B = final ? ~0ull : j.next_lo;
        RunPtr nr = nullptr;
        take_set_.clear();
        for (RunPtr r : src) take_set_.insert(r->page0);
        RC rc = finish_run(*j.w, nr, &wr_merge_);
        if (rc != RC::OK) { abandon_job(); return rc == RC::NOSPACE ? RC::OK : rc; }
        for (auto& t : j.took) { auto it = runs_.find(t.first); if (it != runs_.end()) chain_taken(it->second, t.second.data()); }
        j.took.clear();
        take_set_.clear();
        if (nr) { nr->lvl = j.newlvl; nr->seq = j.maxseq; }
        std::vector<u32> rids;
        for (RunPtr r : src) rids.push_back(r->page0);
        std::vector<Cell*> touched;
        for (auto& kv : cells_) {
            Cell* c = kv.second;
            if (!final && c->hi > B) break;                            // 中途封口：只动 B 以下的单元（B 是单元边界）
            bool hit = nr && (c->hi > nr->first && c->lo <= nr->last);
            for (auto it = c->segs.begin(); it != c->segs.end(); ) {   // 源 run 整条 RUN_DEL / TRIM ⇒ 不写 SEG_DEL（见 merge_run_set）
                if (std::find(rids.begin(), rids.end(), it->second.rid) == rids.end()) { ++it; continue; }
                hit = true;
                RunPtr rr = runs_[it->second.rid];
                seg_set_live(it->second, 0);
                if (rr && rr->refs) --rr->refs;
                it = c->segs.erase(it);
            }
            if (hit) touched.push_back(c);
        }
        std::vector<std::pair<u32, u64>> keep;
        for (size_t i = 0; i < src.size(); ++i) {
            RunPtr r = src[i];
            const bool safe = run_page_safe(r);
            if (final || B > r->last) {                                // 整条归并完 ⇒ 退休
                delta.push_back(run_ent(r, M_RUN_DEL));
                free_ovfl_of(r, safe);
                free_run_pages(r, safe);
                runs_.erase(r->page0);
                delete r;
                continue;
            }
            if (B > r->first) {                                        // 裁前缀
                const u32 tp = page_of(r, B);                          // B 所在页（fences[tp] <= B）：之前的页全是 < B 的死数据
                const u32 lo = std::max<u32>(1, r->trim);
                if (tp > lo) {
                    ensure_ovfl_loc(r);                                // 这些页上还归我们的链（没被接管的：被盖住 / 墓碑 / 悬空副本）先还掉
                    const u32 plo = r->page0 + lo, phi = r->page0 + tp;
                    for (size_t k = 0; k < r->ovfl_loc.size(); ) {
                        const auto e = r->ovfl_loc[k];
                        if (e.first < plo || e.first >= phi) { ++k; continue; }
                        const u8* p = page(e.first);
                        if (p) { OvflDesc d = desc_of(pg_val_at(p, e.second)); if (d.npages && d.pg0 >= 2) free_pages(d.pg0, d.npages, safe); }
                        r->ovfl_loc[k] = r->ovfl_loc.back(); r->ovfl_loc.pop_back();
                    }
                    free_pages(plo, tp - lo, safe);
                    inval(plo, tp - lo);
                    r->trim = tp;
                }
                r->first = B;
                apply_trim_meta(r);
                const u64 v = i < j.src_vis.size() ? j.src_vis[i] : 0;
                r->live_bytes = r->live_bytes > v ? r->live_bytes - v : 0;
                if (r->live_bytes > r->nbytes) r->live_bytes = r->nbytes;
                delta.push_back(trim_ent(r));
                ++run_trims_;
                tr(TR_RETIRE, r->page0, r->trim, (u32)r->seq, 1);
            }
            keep.push_back({r->page0, r->seq});
        }
        j.src.swap(keep);
        j.src_vis.assign(j.src.size(), 0);
        src.clear();
        if (nr) {
            runs_[nr->page0] = nr;
            attach_run(nr);
            delta.push_back(run_ent(nr, M_RUN_ADD));
            if (nr->meta_pending) ensure_meta(nr);
        }
        // 第 14 轮：单元级增量账。第 12 轮在这里把触及的单元全部标未知交给补算（"片与片之间可能有刷出 / 压实 / 拆分改过账"），
        // 实测 wprof 负载补算占维护时间 60%（sweep 2.1 s vs 合并 0.8 s）、mode 1 预算 20 ms 根本追不上写入 ⇒ L0 撞 4t 悬崖。
        // 现在：片归并时按单元累计 em / vis_old（与 merge_run_set 的 CellAcc 同口径），片之后的变化分两类——
        //   (1) 新写盖住了已归并的源赢家：flush_mem 的 probe_prev 本来就要找旧赢家，顺手记到作业账里（job_overwrite：vis_old 扣、sup 加）；
        //   (2) 压实 / 切分 / 范围墓碑 / 盖住的是墓碑：公式不可靠 ⇒ 单元 jdirty，收尾时只对这些单元回退到补算。
        // 产物页还没封时被盖住的记录（late）此刻产物已落地，按产物 probe 补齐字节。
        for (const MergeJob::Late& L : j.late) {
            Cell* c = j.acc[L.ai].c;
            const u8* v = nullptr; u16 len = 0; u32 o = 0;
            if (nr && probe(nr, 0, nr->npages, L.k, ~0ull, v, len, &o) == RC::OK) j.acc[L.ai].sup_new += o;
            else c->jdirty = true;
        }
        if (nr) {
            u64 supb = 0;
            for (const MergeJob::JAcc& a : j.acc) supb += a.sup_new;
            const u64 lb = j.lb > supb ? j.lb - supb : 0;
            nr->live_bytes = lb > nr->nbytes ? nr->nbytes : lb;
        }
        const SegKey nk = nr ? seg_key(nr->seq, nr->page0) : SegKey{};
        for (Cell* c : touched) {
            if (!c->jacc || c->jdirty || !c->acct) {              // 没归并到 / 公式不可靠 / 本来就未知 ⇒ 老路：标未知交给补算
                if (lazy_acct_) { mark_unknown(c); c->pages = 0; for (auto& sg : c->segs) c->pages += sg.second.p1 - sg.second.p0; }
                else recount(c);
                ++job_slow_;
                continue;
            }
            const MergeJob::JAcc& a = j.acc[c->jacc - 1];
            auto it = nr ? c->segs.find(nk) : c->segs.end();
            u64 nb = 0; u32 nrc = 0;
            if (it != c->segs.end()) {
                seg_totals(nr, it->second, c->lo, c->hi, nb, nrc);
                seg_set_live(it->second, a.em >= a.sup ? a.em - a.sup : 0);
            }
            u64 live = c->live >= a.vis_old ? c->live - a.vis_old : 0;
            live += nb >= a.sup_new ? nb - a.sup_new : 0;
            c->live = live;
            const u64 total = total_rec_bytes(c);
            c->dead = total > live ? total - live : 0;
            c->pages = 0;
            for (auto& sg : c->segs) c->pages += sg.second.p1 - sg.second.p0;
            ++job_fast_;
#ifdef FCS_TRACE
            {   // 对账：增量结果必须和全量 recount 完全一致
                const u64 l0 = c->live, d0 = c->dead; const u32 nl = it != c->segs.end() ? it->second.live : 0;
                if (nrc != a.em) std::fprintf(stderr, "  JOB CHECK cell=%llu seg_totals recs=%u emitted=%u\\n", (unsigned long long)c->lo, nrc, a.em);
                recount(c);
                const u32 nl2 = it != c->segs.end() ? it->second.live : 0;
                if (l0 != c->live || d0 != c->dead || nl != nl2)
                    std::fprintf(stderr, "  JOB MISMATCH cell=[%llu,%llu) live %llu vs %llu dead %llu vs %llu newseg.live %u vs %u (em=%u sup=%u vis_old=%llu nb=%llu sup_new=%llu)\\n",
                                 (unsigned long long)c->lo, (unsigned long long)c->hi, (unsigned long long)l0, (unsigned long long)c->live,
                                 (unsigned long long)d0, (unsigned long long)c->dead, nl, nl2, a.em, a.sup, (unsigned long long)a.vis_old,
                                 (unsigned long long)nb, (unsigned long long)a.sup_new);
            }
#endif
        }
        job_clear_acc();
        maint_sync_pending_ = true;
        if (!final) {
            ++merge_parts_;
            j.lb = 0;
#ifdef FCS_TRACE
            std::fprintf(stderr, "  JOB part -> seq=%llu lvl=%u rid=%u pg=%u nrec=%u B=%llu srcs_left=%zu\\n",
                         (unsigned long long)(nr ? nr->seq : 0), j.newlvl, nr ? nr->page0 : 0, nr ? nr->npages : 0, nr ? nr->nrec : 0,
                         (unsigned long long)B, j.src.size());
#endif
            // 下一片：新 RunWriter + 一片大小的预留
            std::vector<RunPtr> rem;
            u64 exp = 0, npg = 0;
            for (auto& e : j.src) { auto it = runs_.find(e.first); if (it != runs_.end()) { rem.push_back(it->second); exp += it->second->nrec; npg += it->second->npages; } }
            if (rem.empty()) { j.done = true; j.w.reset(new RunWriter(j.out_ver, 16)); j.w->st = this; return RC::OK; }
            const u64 per_pg = npg ? (exp + npg - 1) / npg : 64;
            const u32 exp_part = (u32)std::min<u64>(exp, (u64)j.part * per_pg + 16);
            j.w.reset(new RunWriter(j.out_ver, exp_part ? exp_part : 16));
            j.w->st = this;
            j.w->want_kidx = keyidx_;
            const u64 est = stream_cap_est(rem);
            if (est) begin_stream(*j.w, rem, std::min<u64>(est, (u64)j.part + j.part / 8 + 8));
            return RC::OK;
        }
#ifdef FCS_TRACE
        std::fprintf(stderr, "  JOB merge %zu runs -> seq=%llu lvl=%u pg=%u nrec=%u slices=%u moved=%llu tombs_dropped=%llu\\n",
                     rids.size(), (unsigned long long)(nr ? nr->seq : 0), j.newlvl, nr ? nr->npages : 0, nr ? nr->nrec : 0, j.slices,
                     (unsigned long long)j.moved, (unsigned long long)j.dropped_tombs);
#endif
        job_.reset();
        return RC::OK;
    }
""")

# 删除老的落地段（从 "// ---- 4. 落地：与 merge_run_set 的尾段一致" 到函数结束）
splice("""        // ---- 4. 落地：与 merge_run_set 的尾段一致（摘段、退休源 run、挂产物），账目整段重算 ----
        RunPtr nr = nullptr;""",
"""
    // 每层最多 LVL_T 条 run；超了就把最旧的一组升层""",
"")
# 上面 splice 把 "\n    // 每层最多" 前的内容删掉了，但 job_land 的结尾 "    }\n" 已经在 rep 里给出；
# 检查：job_land 之后应直接跟 "\n    // 每层最多 LVL_T"
s = s.replace("        return RC::OK;\n    }\n\n\n    // 每层最多 LVL_T 条 run", "        return RC::OK;\n    }\n\n    // 每层最多 LVL_T 条 run")
assert "        return RC::OK;\n    }\n\n    // 每层最多 LVL_T 条 run" in s

# minkey 不再使用
rep("""                const u64 maxseq = j.maxseq;
                const SegKey minkey = j.minkey;
""", """                const u64 maxseq = j.maxseq;
""")

# ---- 5. level_pass：家族 ----
rep("""                std::sort(set.begin(), set.end(), [](RunPtr a, RunPtr b) { return a->seq < b->seq; });
                // T1（第 12 轮）实验过""",
"""                std::sort(set.begin(), set.end(), [](RunPtr a, RunPtr b) { return a->seq != b->seq ? a->seq < b->seq : a->page0 < b->page0; });
                // P2b：同层同 seq 的 run 是一个"家族"（分片归并的各片 + 同组的最终产物；单源搬家的产物同理）。选组时一个家族当**一条**
                // run 计数 / 算脏度 / 查连续性（gap_between 对相等 seq 不算 gap），选中后展开成 run 列表交给 merge_run_set / start_job。
                // 不这么做：20 片会被 (b) 的 4t 规则当成 20 条 run 纯重写一遍。
                struct Fam { u64 seq; size_t e; u64 nbytes, live, npages; };   // e = 在 set 里的结束下标（不含）
                std::vector<Fam> fam;
                for (size_t i = 0; i < set.size(); ++i) {
                    if (fam.empty() || fam.back().seq != set[i]->seq) fam.push_back(Fam{set[i]->seq, i + 1, 0, 0, 0});
                    Fam& f = fam.back();
                    f.e = i + 1; f.nbytes += set[i]->nbytes; f.live += std::min<u64>(set[i]->live_bytes, set[i]->nbytes); f.npages += set[i]->npages;
                }
                // T1（第 12 轮）实验过""")
rep("""                if (set.empty()) continue;
                // 连续前缀（坑 91）：组只能是全库 seq 顺序上连续的一段
                size_t mcont = 1;
                while (mcont < set.size() && mcont < t * 4 && !gap_between(set[mcont - 1]->seq, set[mcont]->seq)) ++mcont;
                size_t n = 0;                        // 本层要合并的条数（0 = 本层不动）
                bool force1 = false;                 // 单条重写（(d) 长度 1）
                if (set.size() >= t) {""",
"""                if (fam.empty()) continue;
                // 连续前缀（坑 91）：组只能是全库 seq 顺序上连续的一段
                size_t mcont = 1;
                while (mcont < fam.size() && mcont < t * 4 && !gap_between(fam[mcont - 1].seq, fam[mcont].seq)) ++mcont;
                size_t n = 0;                        // 本层要合并的家族数（0 = 本层不动）
                bool force1 = false;                 // 单条重写（(d) 长度 1）
                if (fam.size() >= t) {""")
rep("""                    if (lvl_merges_ && merges >= lvl_merges_ && set.size() < t * 4) continue;""",
"""                    if (lvl_merges_ && merges >= lvl_merges_ && fam.size() < t * 4) continue;""")
rep("""                    size_t k = std::min<size_t>(set.size(), t);
                    u64 tot = 0, live = 0, pg = 0;
                    for (size_t i = 0; i < k; ++i) { tot += set[i]->nbytes; live += set[i]->live_bytes; pg += set[i]->npages; }
                    if (live > tot) live = tot;
                    const bool reclaim = tot && (tot - live) * 100 >= tot * (u64)FCS_LVL_DIRTY_PCT;
                    const bool cheap = cheap_pages_ && pg <= cheap_pages_;
                    if (reclaim || cheap) n = k;
                    else if (set.size() >= t * 4) n = std::min<size_t>(set.size(), t * 4);
                }
                bool dsingle = false;                // (d) 选中了"单条重写"
                if (!n && !set.empty()) {""",
"""                    size_t k = std::min<size_t>(fam.size(), t);
                    u64 tot = 0, live = 0, pg = 0;
                    for (size_t i = 0; i < k; ++i) { tot += fam[i].nbytes; live += fam[i].live; pg += fam[i].npages; }
                    if (live > tot) live = tot;
                    const bool reclaim = tot && (tot - live) * 100 >= tot * (u64)FCS_LVL_DIRTY_PCT;
                    const bool cheap = cheap_pages_ && pg <= cheap_pages_;
                    if (reclaim || cheap) n = k;
                    else if (fam.size() >= t * 4) n = std::min<size_t>(fam.size(), t * 4);
                }
                bool dsingle = false;                // (d) 选中了"单条重写"
                if (!n && !fam.empty()) {""")
rep("""                    for (size_t i = 0; i < kmax; ++i) {
                        tot += set[i]->nbytes; live += std::min<u64>(set[i]->live_bytes, set[i]->nbytes); pg += set[i]->npages;
                        const bool big = i == 0 ? (lvl >= 1 && pg >= 256) : pg >= 64;""",
"""                    for (size_t i = 0; i < kmax; ++i) {
                        tot += fam[i].nbytes; live += fam[i].live; pg += fam[i].npages;
                        const bool big = i == 0 ? (lvl >= 1 && pg >= 256) : pg >= 64;""")
rep("""                if (m < 2) {
                    if (n == 1 && dsingle) force1 = true;   // (d) 单条重写：merge_run_set 单源要 force
                    else {                           // 最老那条被组外 run 隔开：提升到下一层，重来（顶层没有下一层：跳过）
                        if (lvl == LVL_MAX) continue;
                        set[0]->lvl = lvl + 1;
                        did = true;
                        continue;
                    }
                }
                set.resize(m);""",
"""                if (m < 2) {
                    if (n == 1 && dsingle) force1 = true;   // (d) 单条重写：merge_run_set 单源要 force（家族 ≥ 2 条时就是普通多源归并）
                    else {                           // 最老那条被组外 run 隔开：提升到下一层，重来（顶层没有下一层：跳过）
                        if (lvl == LVL_MAX) continue;
                        for (size_t q = 0; q < fam[0].e; ++q) set[q]->lvl = lvl + 1;   // 整个家族一起升
                        did = true;
                        continue;
                    }
                }
                set.resize(fam[m - 1].e);            // 前 m 个家族展开成 run 列表""")

assert s != orig
open(P, 'w', encoding='utf-8').write(s)
print("pass2 ok", len(orig), "->", len(s))
