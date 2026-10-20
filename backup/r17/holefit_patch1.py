# 第 17 轮 ②(c3)：中等产物按洞分片（实验，带开关 set_part_hole_fit）
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

rep("""            if (lbp < FCS_PART_MIN_PAGES) j.part_lo = j.part_hi = 0;
            else {""",
"""            if (lbp < FCS_PART_MIN_PAGES) {
                // 第 17 轮 ②(c3)：中等产物（< 1024 页）默认不分片；但若**没有洞装得下整个产物**、却有洞装得下 ≥ 产物/4 的片
                // ⇒ 按最大洞切成固定片（2 ~ 4 片），否则产物只能长尾 —— layout 200k / 800k 峰值的最后一跳（+289 / +310 页）
                // 正是大合并刚结束、最大洞 262 / 220 页时落下来的 L2 产物（284 ~ 310 页）。片 ≥ FCS_PART_FLOOR_PAGES。
                j.part_lo = j.part_hi = 0;
                if (hole_fit_ && lbp >= 4 * FCS_PART_FLOOR_PAGES) {
                    const u64 best = largest_hole();
                    if (best < part_cap_of(lbp)) {
                        u32 lo = FCS_PART_FLOOR_PAGES, hi = (u32)lbp, p = 0;     // 最大的 p：part_cap_of(p) ≤ best
                        while (lo <= hi) { u32 m = lo + (hi - lo) / 2; if (part_cap_of(m) <= best) { p = m; lo = m + 1; } else { if (m == 0) break; hi = m - 1; } }
                        if (p && (u64)p * 4 >= lbp) { j.part_lo = j.part_hi = p; ++hole_fits_; }
                    }
                }
            } else {""")

rep("""    void choose_part(MergeJob& j) const {
        if (!j.part_hi) { j.part = 0; return; }
        if (j.part_lo >= j.part_hi) { j.part = j.part_hi; return; }     // 固定片（set_merge_part_pages）
        u64 best = 0;
        for (auto& kv : free_) { if (kv.first >= tail_) continue; u64 n = kv.second; if ((u64)kv.first + n > tail_) n = tail_ - kv.first; best = std::max(best, n); }""",
"""    u64 largest_hole() const {                   // 高水位以下最大的空闲洞（页）
        u64 best = 0;
        for (auto& kv : free_) { if (kv.first >= tail_) continue; u64 n = kv.second; if ((u64)kv.first + n > tail_) n = tail_ - kv.first; best = std::max(best, n); }
        return best;
    }
    void choose_part(MergeJob& j) const {
        if (!j.part_hi) { j.part = 0; return; }
        if (j.part_lo >= j.part_hi) { j.part = j.part_hi; return; }     // 固定片（set_merge_part_pages）
        const u64 best = largest_hole();""")

rep("""    u32 part_pages_ = 0;                   // set_merge_part_pages 固定值（part_auto_ = false 时生效；0 = 不分片）
    bool part_auto_ = true;""",
"""    u32 part_pages_ = 0;                   // set_merge_part_pages 固定值（part_auto_ = false 时生效；0 = 不分片）
    bool part_auto_ = true;
    bool hole_fit_ = FCS_PART_HOLE_FIT;    // 第 17 轮 ②(c3)：中等产物按洞分片
    u64 hole_fits_ = 0;""")
rep("""#ifndef FCS_PART_FLOOR_PAGES""",
"""#ifndef FCS_PART_HOLE_FIT
#define FCS_PART_HOLE_FIT 1                // 第 17 轮 ②(c3)：1 = 中等产物（< FCS_PART_MIN_PAGES）装不进任何洞时按最大洞切 2~4 片
#endif
#ifndef FCS_PART_FLOOR_PAGES""")
rep("""    void set_merge_part_pages(u32 n) { part_pages_ = n; part_auto_ = false; }""",
"""    void set_merge_part_pages(u32 n) { part_pages_ = n; part_auto_ = false; }
    void set_part_hole_fit(bool on) { hole_fit_ = on; }        // 第 17 轮 ②(c3)
    u64 part_hole_fits() const { return hole_fits_; }""")
open(P, 'w', encoding='utf-8').write(s)
print("ok")
