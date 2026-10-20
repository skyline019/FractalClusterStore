# 第 17 轮 ② 合并时机规则：三个旋钮（先做成旋钮 + 编译期默认，量了再定默认值）
#   lvl_cliff_      (b) 规则的"悬崖"倍数：fam ≥ cliff·t 才做无垃圾的批量合并（老值 4）
#   lvl_dirty_pct_  (a) 规则的垃圾门槛（老 FCS_LVL_DIRTY_PCT=30，编译期常量 → 运行时旋钮）
#   lvl_dirty_big_pct_  (d) 规则对"大前缀"（≥ FCS_LVL_BIG_PAGES 页）的垃圾门槛（老：一律 50%）
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

rep("""#ifndef FCS_LVL_DIRTY_PCT
#define FCS_LVL_DIRTY_PCT 30  // 分层合并的垃圾门槛（百分比）：够了才值得重写""",
"""#ifndef FCS_LVL_DIRTY_PCT
#define FCS_LVL_DIRTY_PCT 30  // 分层合并的垃圾门槛（百分比）：够了才值得重写（第 17 轮起是运行时旋钮 set_lvl_dirty_pct 的默认值）
#endif
#ifndef FCS_LVL_CLIFF
#define FCS_LVL_CLIFF 4       // 第 17 轮 ②：(b) 规则的"悬崖"——一层攒到 cliff·lvl_t 条（家族）才做无垃圾的批量合并（老值 4，硬编码）
#endif
#ifndef FCS_LVL_BIG_PAGES
#define FCS_LVL_BIG_PAGES 1024   // 第 17 轮 ②：(d) 规则里"大前缀"的页数门槛：≥ 此值的前缀按 lvl_dirty_big_pct_（默认 = (a) 的 30%）而不是 50% 触发
#endif
#ifndef FCS_LVL_DIRTY_BIG_PCT
#define FCS_LVL_DIRTY_BIG_PCT 30""")

# 旋钮
rep("""    void set_level_merges(u32 n) { lvl_merges_ = n; }
    u32 level_merges() const { return lvl_merges_; }""",
"""    void set_level_merges(u32 n) { lvl_merges_ = n; }
    u32 level_merges() const { return lvl_merges_; }
    // 第 17 轮 ②：合并时机的三个旋钮（见 level_pass 的 (a)/(b)/(d)）
    void set_lvl_cliff(u32 n) { lvl_cliff_ = n < 1 ? 1 : n; }                 // (b)：fam ≥ cliff·lvl_t 才批量合并（默认 FCS_LVL_CLIFF）
    void set_lvl_dirty_pct(u32 p) { lvl_dirty_pct_ = p > 100 ? 100 : p; }      // (a)：最老 t 条垃圾占比门槛（默认 FCS_LVL_DIRTY_PCT）
    void set_lvl_dirty_big_pct(u32 p) { lvl_dirty_big_pct_ = p > 100 ? 100 : p; }   // (d)：≥ FCS_LVL_BIG_PAGES 页的前缀的垃圾门槛（默认 FCS_LVL_DIRTY_BIG_PCT）""")

rep("""    u32 lvl_merges_ = 0;                   // R2：每次提交的层合并预算（0 = 不限；见 set_level_merges 的实测）""",
"""    u32 lvl_merges_ = 0;                   // R2：每次提交的层合并预算（0 = 不限；见 set_level_merges 的实测）
    u32 lvl_cliff_ = FCS_LVL_CLIFF;        // 第 17 轮 ②：(b) 悬崖倍数
    u32 lvl_dirty_pct_ = FCS_LVL_DIRTY_PCT;        // (a) 垃圾门槛
    u32 lvl_dirty_big_pct_ = FCS_LVL_DIRTY_BIG_PCT; // (d) 大前缀垃圾门槛""")

# level_pass：t*4 → t*cliff（三处）
rep("""                while (mcont < fam.size() && mcont < t * 4 && !gap_between(fam[mcont - 1].seq, fam[mcont].seq)) ++mcont;""",
    """                const size_t cliff = t * std::max<u32>(lvl_cliff_, 1);   // 第 17 轮 ②：悬崖可调（老值 4t）
                while (mcont < fam.size() && mcont < cliff && !gap_between(fam[mcont - 1].seq, fam[mcont].seq)) ++mcont;""")
rep("""                    if (lvl_merges_ && merges >= lvl_merges_ && fam.size() < t * 4) continue;""",
    """                    if (lvl_merges_ && merges >= lvl_merges_ && fam.size() < cliff) continue;""")
rep("""                    const bool reclaim = tot && (tot - live) * 100 >= tot * (u64)FCS_LVL_DIRTY_PCT;
                    const bool cheap = cheap_pages_ && pg <= cheap_pages_;
                    if (reclaim || cheap) n = k;
                    else if (fam.size() >= t * 4) n = std::min<size_t>(fam.size(), t * 4);""",
"""                    const bool reclaim = tot && (tot - live) * 100 >= tot * (u64)lvl_dirty_pct_;
                    const bool cheap = cheap_pages_ && pg <= cheap_pages_;
                    if (reclaim || cheap) n = k;
                    else if (fam.size() >= cliff) n = std::min<size_t>(fam.size(), cliff);""")
# (d)：大前缀用 lvl_dirty_big_pct_
rep("""                    const size_t kmax = std::min<size_t>(mcont, t);
                    u64 tot = 0, live = 0, pg = 0;
                    long long best = 0;                  // 回收 − 重写 = (tot − live) − live；只接受 ≥ 0（即垃圾 ≥ 50%）
                    for (size_t i = 0; i < kmax; ++i) {
                        tot += fam[i].nbytes; live += fam[i].live; pg += fam[i].npages;
                        const bool big = i == 0 ? (lvl >= 1 && pg >= 256) : pg >= 64;
                        if (!big || !tot) continue;
                        const long long gain = (long long)(tot - live) - (long long)live;
                        if (gain >= best && (tot - live) * 2 >= tot) { best = gain; n = i + 1; dsingle = (i == 0); }
                    }""",
"""                    const size_t kmax = std::min<size_t>(mcont, t);
                    u64 tot = 0, live = 0, pg = 0;
                    long long best = LLONG_MIN;          // 回收 − 重写 = (tot − live) − live；老规则只接受 ≥ 0（即垃圾 ≥ 50%）
                    // 第 17 轮 ②：**大前缀**（≥ FCS_LVL_BIG_PAGES 页）改按 lvl_dirty_big_pct_（默认 30% = (a) 的门槛）触发。
                    // 50% 对小前缀仍合理（绝对垃圾量小、等得起），对顶层那一两条巨型家族就是 2× 的空间天花板：它们不满 t 条、
                    // (a) 永远轮不到，只能等 (d) 的 50%。按 30% 重写的经济性与 (a) 一样（每回收 1 字节重写 2.33 字节），
                    // 空间上限 1.43×。小前缀照旧 50%（gain ≥ 0）。
                    for (size_t i = 0; i < kmax; ++i) {
                        tot += fam[i].nbytes; live += fam[i].live; pg += fam[i].npages;
                        const bool big = i == 0 ? (lvl >= 1 && pg >= 256) : pg >= 64;
                        if (!big || !tot) continue;
                        const long long gain = (long long)(tot - live) - (long long)live;
                        const bool huge = pg >= (u64)FCS_LVL_BIG_PAGES;
                        const bool ok = huge ? (tot - live) * 100 >= tot * (u64)lvl_dirty_big_pct_ : (tot - live) * 2 >= tot;
                        if (ok && gain >= best) { best = gain; n = i + 1; dsingle = (i == 0); }
                    }""")
# mode 3 积压判据
rep("""        const bool b = l0 >= lvl_t_ * 4;""", """        const bool b = l0 >= lvl_t_ * std::max<u32>(lvl_cliff_, 1);""")
if '#include <climits>' not in s:
    rep("#include <cstring>\n", "#include <cstring>\n#include <climits>\n")
open(P, 'w', encoding='utf-8').write(s)
print("ok")
