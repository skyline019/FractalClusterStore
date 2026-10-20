// 叶链 vs 树结构 一致性诊断
#include "store.h"
#include <cstdio>
#include <map>
#include <random>
#include <vector>
#include <set>
#include <algorithm>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];
int main(int argc, char** argv) {
    u32 seed = argc > 1 ? atoi(argv[1]) : 7;
    RamDev dev(8192);
    Engine e;
    e.open(&dev, arena, NAV_CAP);
    std::mt19937_64 rng(seed);
    std::map<u64, u32> oracle;
    for (int i = 0; i < 4500; ++i) {
        u64 k = rng() % 200000 + 1;
        if (rng() % 10 == 0) { e.del(k); oracle.erase(k); }
        else {
            u32 v = (u32)rng();
            u8 b[32]; std::memcpy(b, &v, 4);
            u16 len = (u16)(4 + rng() % 24);
            for (u16 j = 4; j < len; ++j) b[j] = (u8)(v + j);
            if (e.put(k, b, len) == RC::OK) oracle[k] = v;
        }
        if (i % 500 == 499) e.commit();
    }
    e.commit();
    // 1) 树 BFS：收集叶 pid
    std::vector<u16> tree_leaves;
    std::vector<u8> seen(dev.capacity(), 0);
    // 简单做法：从根向下走
    std::vector<u16> stack;
    stack.push_back(e.root());
    while (!stack.empty()) {
        u16 pid = stack.back(); stack.pop_back();
        if (seen[pid]) continue;
        seen[pid] = 1;
        const u8* b = dev.raw(pid);
        if (page_kind(b) == PK_LEAF) { tree_leaves.push_back(pid); continue; }
        IHdr ih; ih_read(b, ih);
        for (u32 i = 0; i < ih.nslots; ++i) { ISlot s; islot_read(b, i, s); stack.push_back(s.child); }
    }
    std::sort(tree_leaves.begin(), tree_leaves.end());
    // 2) 链：从任意叶沿 prev 回到最左，再沿 next
    u16 start = tree_leaves.empty() ? 0xFFFF : tree_leaves[0];
    u16 cur = start;
    for (int g = 0; g < 1000; ++g) {
        const u8* b = dev.raw(cur);
        LHdr h; lh_read(b, h);
        if (h.prev == 0xFFFF) break;
        cur = h.prev;
    }
    std::vector<u16> chain;
    std::set<u16> chain_set;
    for (int g = 0; g < 1000 && cur != 0xFFFF; ++g) {
        if (chain_set.count(cur)) { std::printf("cycle at %u\n", cur); break; }
        chain.push_back(cur); chain_set.insert(cur);
        const u8* b = dev.raw(cur);
        LHdr h; lh_read(b, h);
        cur = h.next;
    }
    std::printf("tree leaves=%zu chain=%zu\n", tree_leaves.size(), chain.size());
    u32 missing = 0, extra = 0;
    for (u16 p : tree_leaves) if (!chain_set.count(p)) { if (missing < 10) std::printf("  tree leaf %u NOT in chain\n", p); ++missing; }
    std::set<u16> tset(tree_leaves.begin(), tree_leaves.end());
    for (u16 p : chain) if (!tset.count(p)) { if (extra < 10) std::printf("  chain leaf %u NOT in tree\n", p); ++extra; }
    std::printf("missing=%u extra=%u\n", missing, extra);
    u32 cnt = 0; e.scan([&](u64, const u8*, u16) { ++cnt; return true; });
    std::printf("scan=%u oracle=%zu\n", cnt, oracle.size());
    u32 miss = 0, missk[4] = {0,0,0,0};
    for (auto& kv : oracle) {
        u8 b[64]; u16 l = 0;
        if (e.get(kv.first, b, sizeof b, &l) != RC::OK) {
            if (miss < 4) missk[miss] = (u32)kv.first;
            ++miss;
        }
    }
    std::printf("get-miss=%u\n", miss);
    for (int q = 0; q < 4 && missk[q]; ++q) {
        u64 key = missk[q];
        std::printf(" key %llu:\n", (unsigned long long)key);
        for (u16 p2 = 2; p2 < dev.capacity(); ++p2) {
            const u8* b = dev.raw(p2);
            if (!page_ok(b) || page_kind(b) != PK_LEAF) continue;
            u32 ns = rd32(b + 4);
            for (u32 unit = 0; unit < ns;) {
                u32 off = slot_off(unit);
                u8 k2 = b[off];
                u32 u = slot_units(k2);
                if (unit + u > ns) break;
                if (k2 == SK_PUB) {
                    Pub pp;
                    if (pub_at(b, off, pp) && pp.key == key)
                        std::printf("   P%u pub ver=%u cred=%u r_off=%u\n", p2, pp.ver, pp.cred, pp.r_off);
                }
                unit += u;
            }
        }
    }
    return 0;
}
