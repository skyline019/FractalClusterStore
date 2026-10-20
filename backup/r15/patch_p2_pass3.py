#!/usr/bin/env python3
"""ROUND15 pass3: trim_run_prefix, apply_trim_geom, upgrade, job hook."""
from pathlib import Path

path = Path("/mnt/e/FCS/embdb/x/include/fcs.h")
text = path.read_text(encoding="utf-8")

def rep(old, new, label):
    global text
    if old not in text:
        print("MISSING:", label)
        return False
    text = text.replace(old, new, 1)
    print("ok:", label)
    return True

TRIM_FUNCS = r'''
    // ---- P2：M_RUN_TRIM ----------------------------------------------------
    // 仅改几何与段表；不释放页（释放由 trim_run_prefix / 重放外的 free_pages 负责）。
    void apply_trim_geom(RunPtr r, u32 new0, u32 new_np, u64 new_first) {
        const u32 old0 = r->page0, old_np = r->npages;
        const u32 drop = new0 - old0;
        if (drop == 0 || new_np == 0 || new0 + new_np != old0 + old_np) return;
        // 段表：rid 改键；丢掉 khi <= new_first 的段；其余收紧
        for (auto& kv : cells_) {
            Cell* c = kv.second;
            std::vector<std::pair<u64, Seg>> keep;
            for (auto it = c->segs.begin(); it != c->segs.end(); ) {
                if (it->second.rid != old0) { ++it; continue; }
                Seg sg = it->second;
                const u64 old_sk = it->first;
                it = c->segs.erase(it);
                if (r->refs) --r->refs;
                c->pages -= sg.p1 - sg.p0;
                if (sg.khi <= new_first) continue;          // 整段在被 trim 掉的前缀键域
                sg.rid = new0;
                if (sg.klo < new_first) sg.klo = new_first;
                // 页下标相对旧 run：减去 drop
                if (sg.p0 >= drop) sg.p0 -= drop; else sg.p0 = 0;
                if (sg.p1 > drop) sg.p1 -= drop; else sg.p1 = 0;
                if (sg.p1 > new_np) sg.p1 = new_np;
                if (sg.p0 >= sg.p1) continue;
                keep.push_back({seg_key(r->seq, new0), sg});
            }
            for (auto& ks : keep) {
                c->segs[ks.first] = ks.second;
                ++r->refs;
                c->pages += ks.second.p1 - ks.second.p0;
            }
            (void)old_sk;
        }
        // fences / 派生数组砍前缀
        if (drop < r->fences.size()) r->fences.erase(r->fences.begin(), r->fences.begin() + drop);
        else r->fences.clear();
        if (drop < r->pbytes.size()) r->pbytes.erase(r->pbytes.begin(), r->pbytes.begin() + drop);
        else r->pbytes.clear();
        if (!r->pnrec.empty()) {
            if (drop < r->pnrec.size()) r->pnrec.erase(r->pnrec.begin(), r->pnrec.begin() + drop);
            else r->pnrec.clear();
        }
        // sp：页号是 run 内相对页，减去 drop；越界条目丢掉
        if (!r->sp.empty()) {
            std::vector<SpEnt> nsp;
            for (auto& e : r->sp) {
                u32 pg = sp_page(e.packed);
                if (pg < drop) continue;
                e.packed = sp_pack(pg - drop, sp_off(e.packed));
                nsp.push_back(e);
            }
            r->sp.swap(nsp);
        }
        runs_.erase(old0);
        r->page0 = new0;
        r->npages = new_np;
        r->first = new_first;
        r->mpage = 0;   // meta 仍在 new0+new_np；下次 ensure_meta 再认
        r->meta_pending.store(1, std::memory_order_relaxed);
        rebuild_pcum(r);
        runs_[new0] = r;
    }

    // 运行时 TRIM：改几何 + 推清单 + 前缀进 pending_free_。快照钉住则跳过。
    RC trim_run_prefix(RunPtr r, u32 drop_pages, std::vector<ManEnt>& delta) {
        if (!r || drop_pages == 0 || drop_pages >= r->npages) return RC::OK;
        const u32 vmin = snap_max();
        if (vmin && r->ver <= vmin) return RC::OK;       // 快照钉住：禁止 TRIM
        const u32 old0 = r->page0;
        const u32 new0 = old0 + drop_pages;
        const u32 new_np = r->npages - drop_pages;
        u64 new_first = r->first;
        if (drop_pages < r->fences.size()) new_first = r->fences[drop_pages];
        ManEnt m;
        m.op = M_RUN_TRIM; m.lvl = (u8)r->lvl;
        m.page0 = old0; m.nbytes = new0; m.npages = new_np;
        m.key = new_first; m.last = r->last; m.ver = r->ver; m.seq = r->seq;
        apply_trim_geom(r, new0, new_np, new_first);
        const bool safe = run_page_safe(r);              // 用 trim 后的 run 判快照安全（页已换身份）
        free_pages(old0, drop_pages, safe);
        delta.push_back(m);
        ++trims_;
        return RC::OK;
    }

    // 片结束后：每条源 run 砍掉已完全落入 [-, hi) 的前缀页。
    void trim_sources_after_slice(std::vector<RunPtr>& src, u64 hi_excl, std::vector<ManEnt>& delta,
                                  std::vector<std::pair<u32, u32>>* job_src) {
        if (hi_excl == 0 || src.empty()) return;
        for (size_t si = 0; si < src.size(); ++si) {
            RunPtr r = src[si];
            if (!r || r->npages < 2 || r->fences.size() < 2) continue;
            u32 drop = 0;
            for (u32 i = 0; i + 1 < r->npages && i + 1 < r->fences.size(); ++i) {
                if (r->fences[i + 1] <= hi_excl) drop = i + 1;
                else break;
            }
            // 最后一页：若 run->last < hi 且整 run 将被吃光，留给 RUN_DEL，不在此 TRIM 成空
            if (drop == 0 || drop >= r->npages) continue;
            const u32 old0 = r->page0;
            if (trim_run_prefix(r, drop, delta) != RC::OK) continue;
            src[si] = r;   // 指针仍有效，page0 已变
            if (job_src) {
                for (auto& e : *job_src)
                    if (e.first == old0 && e.second == (u32)r->seq) e.first = r->page0;
            }
        }
    }

'''

# Insert before ManEnt run_ent
rep(
    "    ManEnt run_ent(RunPtr r, u8 op) {\n",
    TRIM_FUNCS + "    ManEnt run_ent(RunPtr r, u8 op) {\n",
    "trim funcs",
)

# Upgrade at start of durable commit_locked — after ensure_ready / before merged check
rep(
    """        if (wr_gen_ == durable_gen_ && pend_delta_.empty() && gpend_ == 0) { ++commits_merged_; return RC::OK; }
        const u64 my_gen = wr_gen_;""",
    """        if (wr_gen_ == durable_gen_ && pend_delta_.empty() && gpend_ == 0) { ++commits_merged_; return RC::OK; }
        // P2：v1 库首次持久提交 → 全量清单升到 MENT=48 + Sb.fmt=2（避免混宽链）
        if (auto_upgrade_ && disk_fmt_ < FMT_V2) {
            disk_fmt_ = FMT_V2;
            RC ur = write_man_snapshot();
            if (ur != RC::OK) { disk_fmt_ = FMT_V1; dirty_ = true; return ur; }
            pend_.clear();
            promote_pending();
        }
        const u64 my_gen = wr_gen_;""",
    "auto upgrade",
)

# Hook TRIM before return MORE after slice (two places: cut path and normal path)
rep(
    """                if (cut != ~0ull) {                             // 让了路：cut 之后的单元格没归并，退掉它们的作业账（钩子不能对没归并的单元生效）
                    while (!j.acc.empty() && j.acc.back().c->lo >= cut) { j.acc.back().c->jacc = 0; j.acc.back().c->jdirty = false; j.acc.pop_back(); }
                    ++maint_yields_;
                    j.next_lo = cut;
                    return RC::MORE;
                }
                j.next_lo = hi_s;
                if (reached_end || hi_s == ~0ull) j.done = true;
                if (!j.done) return RC::MORE;""",
    """                if (cut != ~0ull) {
                    while (!j.acc.empty() && j.acc.back().c->lo >= cut) { j.acc.back().c->jacc = 0; j.acc.back().c->jdirty = false; j.acc.pop_back(); }
                    ++maint_yields_;
                    j.next_lo = cut;
                    trim_sources_after_slice(src, cut, delta, &j.src);
                    return RC::MORE;
                }
                j.next_lo = hi_s;
                if (reached_end || hi_s == ~0ull) j.done = true;
                if (!j.done) {
                    trim_sources_after_slice(src, hi_s, delta, &j.src);
                    return RC::MORE;
                }""",
    "job TRIM hook",
)

# Fix unused variable warning - remove (void)old_sk by not declaring old_sk
text = text.replace(
    """                const u64 old_sk = it->first;
                it = c->segs.erase(it);
                if (r->refs) --r->refs;
                c->pages -= sg.p1 - sg.p0;
                if (sg.khi <= new_first) continue;          // 整段在被 trim 掉的前缀键域
                sg.rid = new0;
                if (sg.klo < new_first) sg.klo = new_first;
                // 页下标相对旧 run：减去 drop
                if (sg.p0 >= drop) sg.p0 -= drop; else sg.p0 = 0;
                if (sg.p1 > drop) sg.p1 -= drop; else sg.p1 = 0;
                if (sg.p1 > new_np) sg.p1 = new_np;
                if (sg.p0 >= sg.p1) continue;
                keep.push_back({seg_key(r->seq, new0), sg});
            }
            for (auto& ks : keep) {
                c->segs[ks.first] = ks.second;
                ++r->refs;
                c->pages += ks.second.p1 - ks.second.p0;
            }
            (void)old_sk;
        }""",
    """                it = c->segs.erase(it);
                if (r->refs) --r->refs;
                c->pages -= sg.p1 - sg.p0;
                if (sg.khi <= new_first) continue;
                sg.rid = new0;
                if (sg.klo < new_first) sg.klo = new_first;
                if (sg.p0 >= drop) sg.p0 -= drop; else sg.p0 = 0;
                if (sg.p1 > drop) sg.p1 -= drop; else sg.p1 = 0;
                if (sg.p1 > new_np) sg.p1 = new_np;
                if (sg.p0 >= sg.p1) continue;
                keep.push_back({seg_key(r->seq, new0), sg});
            }
            for (auto& ks : keep) {
                c->segs[ks.first] = ks.second;
                ++r->refs;
                c->pages += ks.second.p1 - ks.second.p0;
            }
        }""",
)

# Fix job_src seq compare: e.second is u32 in MergeJob - check MergeJob src type
path.write_text(text, encoding="utf-8")
print("WROTE pass3")
