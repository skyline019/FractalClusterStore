# 第 17 轮 ③ 家族"虚拟源"游标：同 seq 的片（P2b 家族）串成一条游标顺序读；源下标不再限 64（按单元的源下标表）；归并宽度统计
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

# ---------- 1. Cur：链 ----------
rep("""        bool advance(Store* s, u16 len) { off += pg_rec_size(pfx, len); return fixup(s); }
        void hdr(RecHdr& h) const { rd_rec(buf.data(), off, h); }""",
"""        bool advance(Store* s, u16 len) { off += pg_rec_size(pfx, len); return fixup(s); }
        void hdr(RecHdr& h) const { rd_rec(buf.data(), off, h); }
        // 第 17 轮 ③：家族"虚拟源"。P2b 之后同 seq 的 run（分片产物的各片 / 裁过的源 + 它的片）键域两两不相交、按 first 排好，
        // 归并时串成**一条**游标顺序读：当前片读完（fixup 走到 p1）就换下一片。归并宽度 = 家族数（≤ cliff·t）而不是片数
        //（≤ cliff·t × 64），每条游标一页缓冲、每条记录的赢家扫描 O(家族数)。链由调用方持有（chain 只是指针）。
        // slo/shi：链上每一片的页范围按这个键区间算（作业分片时 = 本片键区间；整条归并时 = 全域）。
        const std::vector<RunPtr>* chain = nullptr;
        size_t ci = 0;
        u64 slo = 0, shi = ~0ull;
        void open_chain(Store* s, const std::vector<RunPtr>* ch, u64 lo, u64 hi, u64 hi_key) {
            chain = ch; ci = ~(size_t)0; slo = lo; shi = hi; khi = hi_key; done = false;
            next_run(s);
        }
        bool next_run(Store* s) {
            while (chain && ci + 1 < chain->size()) {
                ++ci;
                r = (*chain)[ci];
                page_range(r, slo, shi, p0, p1);
                klo = std::max(slo, r->first);             // 裁过的片：混合页上 < first 的死副本不归并（merge_run_set 同款）
                if (p0 >= p1) continue;
                done = false;
                if (!load(s, klo)) { if (done && chain && ci + 1 < chain->size()) continue; return !done; }
                RecHdr h;                                   // 跳过 < klo 的记录（本片的死前缀；merge_emit 开头的 seek 只对第一片做过）
                while (!done && (hdr(h), h.key < klo)) if (!advance(s, h.len)) break;
                return !done;
            }
            done = true;
            return false;
        }""")

rep("""                if (++pi >= p1) { done = true; return false; }
                // 下一页的首键已经越过本段键域 ⇒ 不必把它读进来（fence 在内存里）
                if (pi < r->fences.size() && r->fences[pi] >= khi) { done = true; return false; }""",
"""                if (++pi >= p1) { if (chain) return next_run(s); done = true; return false; }   // 第 17 轮 ③：链上还有片就换片
                // 下一页的首键已经越过本段键域 ⇒ 不必把它读进来（fence 在内存里）。链上后面的片键更大，同样越界 ⇒ 整条游标完。
                if (pi < r->fences.size() && r->fences[pi] >= khi) { done = true; return false; }""")

# Cur 里用到 page_range（static 成员，定义在 Cur 之后）：前置声明不可行（同一个类内成员函数体在类定义完成后才编译）⇒ 无需处理。

# ---------- 2. 建链 ----------
rep("""    // 多段归并输出（scan / compact 共用）：""",
"""    // 第 17 轮 ③：把源 run 列表按 seq 分组成"家族链"：同 seq 且键域两两不相交（按 first 排序后 first_i > last_{i-1}）的 run 串成一条；
    // 不满足（理论上不会：同 seq 只来自分片 / 搬家 / 裁源）就各自单独成链（退化为老行为）。只收与 [lo, hi) 相交的 run。
    // 返回的链按 (seq, first) 排序；每条链 ≥ 1 条 run。
    static void build_chains(const std::vector<RunPtr>& src, std::vector<std::vector<RunPtr>>& out, u64 lo = 0, u64 hi = ~0ull) {
        out.clear();
        std::vector<RunPtr> v;
        v.reserve(src.size());
        for (RunPtr r : src) if (r && r->npages && r->last >= lo && r->first < hi) v.push_back(r);
        std::sort(v.begin(), v.end(), [](RunPtr a, RunPtr b) { return a->seq != b->seq ? a->seq < b->seq : (a->first != b->first ? a->first < b->first : a->page0 < b->page0); });
        for (size_t i = 0; i < v.size();) {
            size_t j = i + 1;
            while (j < v.size() && v[j]->seq == v[i]->seq) ++j;
            bool disjoint = true;
            for (size_t q = i + 1; q < j && disjoint; ++q) if (v[q]->first <= v[q - 1]->last) disjoint = false;
            if (disjoint) out.emplace_back(v.begin() + i, v.begin() + j);
            else for (size_t q = i; q < j; ++q) out.emplace_back(1, v[q]);
            i = j;
        }
    }
    // 多段归并输出（scan / compact 共用）：""")

# ---------- 3. merge_run_set：链游标 + 源下标表 ----------
rep("""    RC merge_run_set(std::vector<RunPtr>& src, std::vector<ManEnt>& delta, u32 newlvl, bool force = false, bool stream = true) {
        std::vector<Cur> cs;
        cs.reserve(src.size());
        u32 exp = 0; u64 out_ver = 0, maxseq = 0;
        u64 kmin = ~0ull, kmax = 0; SegKey minkey{~0ull, ~0u};    // minkey：组内最老 run 的 seg_key（坑 94 的\"更老\"分界）
        for (RunPtr r : src) {
            if (!r || r->npages == 0) continue;
            exp += r->nrec;
            out_ver = std::max(out_ver, r->ver);
            maxseq = std::max(maxseq, r->seq);
            minkey = std::min(minkey, seg_key(r->seq, r->page0));
            kmin = std::min(kmin, r->first);
            kmax = std::max(kmax, r->last);
            cs.emplace_back();
            Cur& c = cs.back();
            c.r = r; c.p0 = r->trim; c.p1 = r->npages; c.klo = r->first; c.khi = ~0ull;   // P2b：裁过的源：混合页上 < first 的死副本不归并
            c.load(this);
        }
        if (cs.size() < 2 && !force) return RC::OK;      // force ⇒ 单条 run 也重写（搬家）""",
"""    RC merge_run_set(std::vector<RunPtr>& src, std::vector<ManEnt>& delta, u32 newlvl, bool force = false, bool stream = true) {
        std::vector<Cur> cs;
        u32 exp = 0; u64 out_ver = 0, maxseq = 0;
        u64 kmin = ~0ull, kmax = 0; SegKey minkey{~0ull, ~0u};    // minkey：组内最老 run 的 seg_key（坑 94 的\"更老\"分界）
        size_t nsrc = 0;
        for (RunPtr r : src) {
            if (!r || r->npages == 0) continue;
            ++nsrc;
            exp += r->nrec;
            out_ver = std::max(out_ver, r->ver);
            maxseq = std::max(maxseq, r->seq);
            minkey = std::min(minkey, seg_key(r->seq, r->page0));
            kmin = std::min(kmin, r->first);
            kmax = std::max(kmax, r->last);
        }
        // 第 17 轮 ③：同 seq 的片串成一条虚拟源（见 Cur::next_run / build_chains）
        std::vector<std::vector<RunPtr>> chains;
        build_chains(src, chains);
        cs.reserve(chains.size());
        for (auto& ch : chains) { cs.emplace_back(); cs.back().open_chain(this, &ch, 0, ~0ull, ~0ull); }
        merge_src_max_ = std::max<u64>(merge_src_max_, nsrc);
        merge_cur_max_ = std::max<u64>(merge_cur_max_, cs.size());
        if (nsrc < 2 && !force) return RC::OK;           // force ⇒ 单条 run 也重写（搬家）""")

rep("""        struct CellAcc { Cell* c; u64 smask; u32 em; u32 sup; u64 vis_old; u64 sup_new; };
        std::vector<CellAcc> acc;                   // 覆盖 [kmin, kmax] 的连续单元
        const bool fast = src.size() <= 64;         // 源 run 索引用 64 位掩码；更多就退回 recount（不会发生：lvl_t_*4）
        {
            auto cit = cells_.upper_bound(kmin);
            if (cit != cells_.begin()) --cit;
            for (; cit != cells_.end() && cit->second->lo <= kmax; ++cit) {
                Cell* c = cit->second;
                CellAcc a{c, 0, 0, 0, 0, 0};
                for (size_t i = 0; i < src.size() && fast; ++i)
                    if (src[i] && c->segs.count(seg_key(src[i]->seq, src[i]->page0))) a.smask |= 1ull << i;
                acc.push_back(a);
            }
        }""",
"""        // 第 17 轮 ③：源下标不再限 64 条（家族展开后源可达 cliff·t × 64）。每个单元记"在本单元有段的源下标"（有序小表：一个单元
        // 每个家族至多一片有段 ⇒ ≤ 家族数），按段表建（O(段) 而不是 O(源)）；源 run → 下标用哈希表（以前逐条线性找）。
        struct CellAcc { Cell* c; std::vector<u32> sidx; u32 em; u32 sup; u64 vis_old; u64 sup_new; };
        std::vector<CellAcc> acc;                   // 覆盖 [kmin, kmax] 的连续单元
        const bool fast = true;
        std::unordered_map<u32, u32> sidx_of;       // 源 page0 → src 下标
        for (size_t i = 0; i < src.size(); ++i) if (src[i]) sidx_of[src[i]->page0] = (u32)i;
        {
            auto cit = cells_.upper_bound(kmin);
            if (cit != cells_.begin()) --cit;
            for (; cit != cells_.end() && cit->second->lo <= kmax; ++cit) {
                Cell* c = cit->second;
                CellAcc a{c, {}, 0, 0, 0, 0};
                for (auto& sg : c->segs) { auto f = sidx_of.find(sg.second.rid); if (f != sidx_of.end() && src[f->second]->seq == sg.first.seq) a.sidx.push_back(f->second); }
                std::sort(a.sidx.begin(), a.sidx.end());
                acc.push_back(std::move(a));
            }
        }
        auto has_src = [](const CellAcc& a, size_t si) { return std::binary_search(a.sidx.begin(), a.sidx.end(), (u32)si); };""")

rep("""        auto src_index = [&](RunPtr r) -> size_t { for (size_t i = 0; i < src.size(); ++i) if (src[i] == r) return i; return 64; };
        struct Pend { u64 k; u32 osz; u32 ci; bool vis; bool dropped; };""",
"""        auto src_index = [&](RunPtr r) -> size_t { auto f = sidx_of.find(r->page0); return f != sidx_of.end() ? f->second : src.size(); };
        struct Pend { u64 k; u32 osz; u32 ci; bool vis; bool dropped; };""")

rep("""            if (a && a->smask == 0) return true;
            bool vis = false;
            if (a) { size_t si = sr ? src_index(sr) : 64; vis = si < 64 && ((a->smask >> si) & 1ull); }""",
"""            if (a && a->sidx.empty()) return true;
            bool vis = false;
            if (a) { size_t si = sr ? src_index(sr) : src.size(); vis = si < src.size() && has_src(*a, si); }""")

# ---------- 4. merge_job_step：链游标 + 源下标表 ----------
rep("""                std::vector<Cur> cs;
                cs.reserve(src.size());
                for (RunPtr r : src) {
                    cs.emplace_back();
                    Cur& c = cs.back();
                    c.r = r; c.klo = lo_s; c.khi = hi_s;
                    page_range(r, lo_s, hi_s, c.p0, c.p1);
                    c.load(this, lo_s);
                }
                struct CA { Cell* c; u64 smask; u32 ai; };
                std::vector<CA> acc;                  // 片内单元：源 run 在该单元有没有段（没有 ⇒ 记录全是悬空副本，不搬）；ai = 作业级账的下标
                {
                    auto it2 = cells_.upper_bound(lo_s);
                    if (it2 != cells_.begin()) --it2;
                    for (; it2 != cells_.end() && it2->second->lo < hi_s; ++it2) {
                        if (it2->second->hi <= lo_s) continue;
                        CA a{it2->second, 0, 0};
                        for (size_t i = 0; i < src.size() && i < 64; ++i)
                            if (a.c->segs.count(seg_key(src[i]->seq, src[i]->page0))) a.smask |= 1ull << i;
                        if (!a.c->jacc) { j.acc.push_back(MergeJob::JAcc{a.c, 0, 0, 0, 0}); a.c->jacc = (u32)j.acc.size(); }
                        a.ai = a.c->jacc - 1;
                        acc.push_back(a);
                    }
                }
                auto src_index = [&](RunPtr r) -> size_t { for (size_t i = 0; i < src.size(); ++i) if (src[i] == r) return i; return 64; };""",
"""                // 第 17 轮 ③：本片只给"与片相交的 run"开游标，同 seq 的片串成一条虚拟源（见 build_chains / Cur::open_chain）
                std::vector<std::vector<RunPtr>> chains;
                build_chains(src, chains, lo_s, hi_s);
                std::vector<Cur> cs;
                cs.reserve(chains.size());
                for (auto& ch : chains) { cs.emplace_back(); cs.back().open_chain(this, &ch, lo_s, hi_s, hi_s); }
                merge_src_max_ = std::max<u64>(merge_src_max_, src.size());
                merge_cur_max_ = std::max<u64>(merge_cur_max_, cs.size());
                std::unordered_map<u32, u32> sidx_of;         // 源 page0 → src 下标（源下标不再限 64）
                for (size_t i = 0; i < src.size(); ++i) sidx_of[src[i]->page0] = (u32)i;
                struct CA { Cell* c; std::vector<u32> sidx; u32 ai; };
                std::vector<CA> acc;                  // 片内单元：在该单元有段的源下标（空 ⇒ 记录全是悬空副本，不搬）；ai = 作业级账的下标
                {
                    auto it2 = cells_.upper_bound(lo_s);
                    if (it2 != cells_.begin()) --it2;
                    for (; it2 != cells_.end() && it2->second->lo < hi_s; ++it2) {
                        if (it2->second->hi <= lo_s) continue;
                        CA a{it2->second, {}, 0};
                        for (auto& sg : a.c->segs) { auto f = sidx_of.find(sg.second.rid); if (f != sidx_of.end() && src[f->second]->seq == sg.first.seq) a.sidx.push_back(f->second); }
                        std::sort(a.sidx.begin(), a.sidx.end());
                        if (!a.c->jacc) { j.acc.push_back(MergeJob::JAcc{a.c, 0, 0, 0, 0}); a.c->jacc = (u32)j.acc.size(); }
                        a.ai = a.c->jacc - 1;
                        acc.push_back(std::move(a));
                    }
                }
                auto src_index = [&](RunPtr r) -> size_t { auto f = sidx_of.find(r->page0); return f != sidx_of.end() ? f->second : src.size(); };
                auto has_src = [](const CA& a, size_t si) { return std::binary_search(a.sidx.begin(), a.sidx.end(), (u32)si); };""")

rep("""                    if (a && a->smask == 0) return true;   // 悬空副本：不搬（见 merge_run_set 同款说明）
                    bool vis = false;                      // 源副本以前对读可见（来源 run 在本单元有段）
                    size_t si = 64;
                    if (a && sr) { si = src_index(sr); vis = si < 64 && ((a->smask >> si) & 1ull); }""",
"""                    if (a && a->sidx.empty()) return true;   // 悬空副本：不搬（见 merge_run_set 同款说明）
                    bool vis = false;                      // 源副本以前对读可见（来源 run 在本单元有段）
                    size_t si = src.size();
                    if (a && sr) { si = src_index(sr); vis = si < src.size() && has_src(*a, si); }""")

# ---------- 5. 统计 ----------
rep("""                        u64 sweep_us, rebal_us, level_us, recounts, job_fast, job_slow, yields, unit_max_us, unit_long, stream_extends, hw_reclaims, hw_blockers; };""",
    """                        u64 sweep_us, rebal_us, level_us, recounts, job_fast, job_slow, yields, unit_max_us, unit_long, stream_extends, hw_reclaims, hw_blockers;
                        u64 merge_src_max, merge_cur_max; };   // 第 17 轮 ③：一次归并最多几条源 run / 几条游标（家族虚拟源后游标数 = 家族数）""")
old_ms = s[s.find("    MaintStats maint_stats() const { return MaintStats{"):]
old_ms = old_ms[:old_ms.find("}; }") + 4]
assert old_ms.count("hw_blockers_") == 1, old_ms
new_ms = old_ms.replace("}; }", ",\n                                                       merge_src_max_, merge_cur_max_}; }")
rep(old_ms, new_ms)
rep("""    u32 lvl_cliff_ = FCS_LVL_CLIFF;        // 第 17 轮 ②：(b) 悬崖倍数""",
    """    u64 merge_src_max_ = 0, merge_cur_max_ = 0;   // 第 17 轮 ③：归并宽度统计
    u32 lvl_cliff_ = FCS_LVL_CLIFF;        // 第 17 轮 ②：(b) 悬崖倍数""")
if '#include <unordered_map>' not in s:
    rep("#include <map>\n", "#include <map>\n#include <unordered_map>\n")
open(P, 'w', encoding='utf-8').write(s)
print("ok")
