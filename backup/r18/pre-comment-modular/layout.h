// XDB v2 —— 页面布局（位置化原子格式）
//
// 每一页 = 12B 通用头 + 固定位置区（锚/头/元/尾） + 增长区（值向上、槽向下） + 导航带。
// 热计数器一律 4B 对齐：任何元数据/指针的更新都是「单次 4B 对齐写」，不触发读-改-写。
//
// 叶页（PAGE 字节）
//   [0..12)      通用头 {kind, flags, seq, nslots(u32), phash(u32)}
//   [96..160)    锚 V1（64B，建页时一次写入，此后永不变更；union-of-formats 的稳定成员）
//   [160..208)   叶头（44B 有效）
//   [208..336)   链群摘要 meta[8]（16B/群）
//   [336..400)   提交尾（64B，每次提交只写变化的 4B 字）
//   [400, cur)   值/大原子区（向上）
//   [cur, SLOT_TOP-4*nslots) 空闲
//   [SLOT_TOP-4*nslots, SLOT_TOP) 槽/发布区（向下增长；新增槽位于末尾=最新）
//   [SLOT_TOP, PAGE)          导航带 NAV（64 个 4B 单元）
//
// 槽格式（全部 4B 对齐、自校验 chk、可独立成链）
//   PUB20  : {kind=0, chk, ver u16, cred u32, key u64, r_off u32}
//   VALHDR : {kind=1, chk, len u16}           值块头（二级：槽 → 头 → 字节）
//   LEAFINC: {kind=2, chk, ...} 16B 内容        批次/滚动的增量成员（V1 覆盖区内）
//   TOMB   : PUB20 且 r_off==0
//
// 内页
//   [160..192)   内头 {nid u64, roll u32, nslots u16, level u16, ceil u16, sealed_base u16, sealed_cnt u16, b_vers u16, pad u32}
//   槽 16B：{key u64, child u16, pad u16, chk u32}，从 SLOT_TOP 向下
#pragma once
#include "common.h"

namespace xdb {

// ------------------------------------------------------------------ 常量
enum : u32 {
    LEAF_ANCHOR = 96,
    LEAF_HDR    = 160,
    LEAF_META   = 208,
    LEAF_FOOT   = 336,
    LEAF_DATA0  = 400,          // 槽区起点（向上增长）
    SLOT_CUR0   = PAGE - 256,   // 值区起点（向下增长），与 SLOT_TOP 相同
    INNER_HDR   = 160,
    MAP_REC     = 96,
    SLOT_TOP    = PAGE - 256,      // 槽区上界；[SLOT_TOP, PAGE) 为导航带
    NAV_SLOTS   = 64,
    META_CAP    = 8,
    PUB_SZ      = 20,
    VALHDR_SZ   = 4,
    LEAFINC_SZ  = 16,
    INNER_SLOT  = 16,
};

enum : u8 { PK_FREE = 0, PK_LEAF = 1, PK_MAP = 2, PK_INNER = 3 };
enum : u8 { SK_PUB = 0, SK_VALHDR = 1, SK_LEAFINC = 2 };
enum : u32 { MAGIC_ANCHOR = 0x414E4358u };   // 'X','C','N','A'
enum : u32 { MAGIC_MAP = 0x4D424458u };      // 'X','D','B','M'
enum : u16 { MAGIC_FREE = 0x5246u };

// ------------------------------------------------------------------ 基础读写字
enum : u16 { MAP_A = 0, MAP_B = 1 };
inline u16 rd16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
inline u32 rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
inline u64 rd64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline void wr16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
inline void wr32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
inline void wr64(u8* p, u64 v) { std::memcpy(p, &v, 8); }

// 页哈希：覆盖 [0,8) 与 [12,PAGE)，存于 [8,12)（4B 对齐单写）
inline u32 page_hash(const u8* pg) {
    u64 h = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < PAGE; i += 8) {
        if (i == 8) continue;              // 跳过 phash 自身
        h ^= rd64(pg + i);
        h = (h << 7) | (h >> 57);
        h *= 0x100000001B3ULL;
    }
    return (u32)(h ^ (h >> 32));
}
inline void page_seal(u8* pg) { wr32(pg + 8, page_hash(pg)); }
inline bool page_ok(const u8* pg) { return rd32(pg + 8) == page_hash(pg); }
inline u8 page_kind(const u8* pg) { return pg[0]; }
inline void page_init(u8* pg, u8 kind) {
    std::memset(pg, 0, PAGE);
    pg[0] = kind;
    wr16(pg + 2, 1);
}

// ------------------------------------------------------------------ 叶头
struct LHdr {
    u64 nid;
    u64 min_key;      // 只降不升的安全下界
    u16 next, prev;
    u16 meta_cnt, meta_cap;
    u16 data_off;     // 已提交的值区游标
    u16 close_vers;   // 已提交版本
    u16 b_vers;       // 未提交批的版本（0=无）
    u16 maxu;
    u16 flags2;
};
inline void lh_read(const u8* pg, LHdr& h) {
    const u8* p = pg + LEAF_HDR;
    h.nid = rd64(p);
    h.min_key = rd64(p + 8);
    h.next = rd16(p + 16);
    h.prev = rd16(p + 18);
    h.meta_cnt = rd16(p + 20);
    h.meta_cap = rd16(p + 22);
    h.data_off = rd16(p + 24);
    h.close_vers = rd16(p + 26);
    h.b_vers = rd16(p + 28);
    h.maxu = rd16(p + 30);
    h.flags2 = rd16(p + 32);
}
inline void lh_write(u8* pg, const LHdr& h) {
    u8* p = pg + LEAF_HDR;
    wr64(p, h.nid);
    wr64(p + 8, h.min_key);
    wr16(p + 16, h.next);
    wr16(p + 18, h.prev);
    wr16(p + 20, h.meta_cnt);
    wr16(p + 22, h.meta_cap);
    wr16(p + 24, h.data_off);
    wr16(p + 26, h.close_vers);
    wr16(p + 28, h.b_vers);
    wr16(p + 30, h.maxu);
    wr16(p + 32, h.flags2);
}

// 链群摘要
struct Meta { u64 min_key; u32 roll; u16 start, cnt; };
inline void meta_read(const u8* pg, u16 r, Meta& m) {
    const u8* p = pg + LEAF_META + 16u * r;
    m.min_key = rd64(p);
    m.roll = rd32(p + 8);
    m.start = rd16(p + 12);
    m.cnt = rd16(p + 14);
}
inline void meta_write(u8* pg, u16 r, const Meta& m) {
    u8* p = pg + LEAF_META + 16u * r;
    wr64(p, m.min_key);
    wr32(p + 8, m.roll);
    wr16(p + 12, m.start);
    wr16(p + 14, m.cnt);
}

// 提交尾（页级 redo 记录，固定 64B，4B 对齐访问）
struct Foot {
    u16 vers;
    u32 nslots;
    u32 data_off;
    u32 rolls[META_CAP];
    u64 min_key;
};
inline void foot_read(const u8* pg, Foot& f) {
    const u8* p = pg + LEAF_FOOT;
    f.vers = rd16(p + 2);
    f.nslots = rd32(p + 4);
    f.data_off = rd32(p + 8);
    for (u32 i = 0; i < META_CAP; ++i) f.rolls[i] = rd32(p + 12 + 4 * i);
    f.min_key = rd64(p + 44);
}
inline void foot_write_prefix(u8* pg, const Foot& f) {
    u8* p = pg + LEAF_FOOT;
    p[0] = 0xE2;
    wr16(p + 2, f.vers);
    wr32(p + 4, f.nslots);
    wr32(p + 8, f.data_off);
    for (u32 i = 0; i < META_CAP; ++i) wr32(p + 12 + 4 * i, f.rolls[i]);
    wr64(p + 44, f.min_key);
}
inline void foot_seal(u8* pg) {
    u8* p = pg + LEAF_FOOT;
    u16 h = atom_hash(p + 2, 62, 0x77u);
    p[1] = (u8)h;
    page_seal(pg);
}
inline bool foot_ok(const u8* pg) {
    const u8* p = pg + LEAF_FOOT;
    if (p[0] != 0xE2) return false;
    u16 h = atom_hash(p + 2, 62, 0x77u);
    return p[1] == (u8)h;
}

// ------------------------------------------------------------------ 槽访问
struct Pub {
    const u8* p;        // 槽位置
    u16 ver;
    u32 cred;
    u64 key;
    u32 r_off;          // 0 = 墓碑
    u32 size;           // 本槽占用的 4B 单元数
};
inline u8 pub_chk(u8 kind, u16 ver, u32 cred, u64 key, u32 r_off) {
    u32 h = mix32((u32)key ^ (u32)(key >> 32) ^ (u32)ver * 0x9E3779B9u ^ cred * 0x85EBCA6Bu ^ (r_off * 0xC2B2AE35u) ^ kind);
    return (u8)(h ^ (h >> 8));
}
inline u32 slot_units(u8 kind) {
    switch (kind) {
        case SK_PUB: return PUB_SZ / 4;
        case SK_VALHDR: return VALHDR_SZ / 4;
        case SK_LEAFINC: return LEAFINC_SZ / 4;
        default: return 1;    // 未知：单字跳过（自愈）
    }
}
inline u32 slot_off(u32 unit) { return LEAF_DATA0 + 4u * unit; }   // 槽向上增长

inline bool pub_at(const u8* pg, u32 off, Pub& out) {
    const u8* p = pg + off;
    u8 kind = p[0];
    if (kind != SK_PUB) return false;
    u16 ver = rd16(p + 2);
    u32 cred = rd32(p + 4);
    u64 key = rd64(p + 8);
    u32 r_off = rd32(p + 16);
    if (p[1] != pub_chk(kind, ver, cred, key, r_off)) return false;
    if (r_off && r_off + VALHDR_SZ > SLOT_TOP) return false;
    out.p = p; out.ver = ver; out.cred = cred; out.key = key; out.r_off = r_off; out.size = PUB_SZ / 4;
    return true;
}
inline void pub_write(u8* pg, u32 off, u16 ver, u32 cred, u64 key, u32 r_off) {
    u8* p = pg + off;
    p[0] = SK_PUB;
    p[1] = pub_chk(SK_PUB, ver, cred, key, r_off);
    wr16(p + 2, ver);
    wr32(p + 4, cred);
    wr64(p + 8, key);
    wr32(p + 16, r_off);
}
inline void valhdr_write(u8* pg, u32 off, u16 len) {
    u8* p = pg + off;
    p[0] = SK_VALHDR;
    p[1] = (u8)atom_hash((const u8*)&len, 2, 0x91u);
    wr16(p + 2, len);
}
inline bool valhdr_ok(const u8* pg, u32 off, u16 len) {
    const u8* p = pg + off;
    return p[0] == SK_VALHDR && rd16(p + 2) == len && p[1] == (u8)atom_hash(&p[2], 2, 0x91u);
}

// 顺序遍历槽区：oldest → newest
struct SlotIter {
    const u8* pg;
    u32 unit, end, off;
    void begin(const u8* page, u32 nslots) {
        pg = page; unit = 0; end = nslots; off = slot_off(0);
    }
    u8 next(u32& out_off, u32& out_units) {
        if (unit >= end) return 0xFF;
        u8 kind = pg[off];
        u32 n = slot_units(kind);
        if (unit + n > end) { unit = end; return 0xFF; }
        out_off = off; out_units = n;
        unit += n;
        off = slot_off(unit);
        return kind;
    }
};

// ------------------------------------------------------------------ 内页
struct IHdr {
    u64 nid;
    u32 roll;
    u16 nslots;
    u16 level;
    u16 ceil;
    u16 sealed_base;
    u16 sealed_cnt;
    u16 b_vers;
};
inline void ih_read(const u8* pg, IHdr& h) {
    const u8* p = pg + INNER_HDR;
    h.nid = rd64(p);
    h.roll = rd32(p + 8);
    h.nslots = rd16(p + 12);
    h.level = rd16(p + 14);
    h.ceil = rd16(p + 16);
    h.sealed_base = rd16(p + 18);
    h.sealed_cnt = rd16(p + 20);
    h.b_vers = rd16(p + 22);
}
inline void ih_write(u8* pg, const IHdr& h) {
    u8* p = pg + INNER_HDR;
    wr64(p, h.nid);
    wr32(p + 8, h.roll);
    wr16(p + 12, h.nslots);
    wr16(p + 14, h.level);
    wr16(p + 16, h.ceil);
    wr16(p + 18, h.sealed_base);
    wr16(p + 20, h.sealed_cnt);
    wr16(p + 22, h.b_vers);
}
struct ISlot { u64 key; u16 child; };
enum : u32 { INNER_DATA0 = 400 };
inline u32 islot_off(u32 i) { return INNER_DATA0 + INNER_SLOT * i; }   // 向内页上方增长
inline void islot_read(const u8* pg, u32 i, ISlot& s) {
    const u8* p = pg + islot_off(i);
    s.key = rd64(p);
    s.child = rd16(p + 8);
}
inline u32 islot_chk(u64 key, u16 child) { return mix32((u32)key ^ (u32)(key >> 32) ^ ((u32)child << 13)); }
inline void islot_write(u8* pg, u32 i, u64 key, u16 child) {
    u8* p = pg + islot_off(i);
    wr64(p, key);
    wr16(p + 8, child);
    wr32(p + 12, islot_chk(key, child));
}
inline bool islot_ok(const u8* pg, u32 i, ISlot& s) {
    const u8* p = pg + islot_off(i);
    s.key = rd64(p);
    s.child = rd16(p + 8);
    return rd32(p + 12) == islot_chk(s.key, s.child);
}

// ------------------------------------------------------------------ 映射页
struct MapRec {
    u64 nid;
    u16 state;        // 0xA55A 有效；0xFFFF 已擦除；0x0000 无效
    u16 root;
    u16 acc;
    u16 batch_n;
    u16 c_hint;
    u32 next_vers;
    u16 cand[CAND_CAP];
    u16 ring[64];     // 版本 → 根（时间旅行的持久历史环）
};
inline void map_read(const u8* pg, MapRec& m) {
    const u8* p = pg + MAP_REC;
    m.nid = rd64(p);
    m.state = rd16(p + 8);
    m.root = rd16(p + 10);
    m.acc = rd16(p + 12);
    m.batch_n = rd16(p + 14);
    m.c_hint = rd16(p + 16);
    m.next_vers = rd32(p + 20);
    for (u32 i = 0; i < CAND_CAP; ++i) m.cand[i] = rd16(p + 24 + 2 * i);
    for (u32 i = 0; i < 64; ++i) m.ring[i] = rd16(p + 24 + 2 * CAND_CAP + 2 * i);
}
inline void map_write(u8* pg, const MapRec& m) {
    u8* p = pg + MAP_REC;
    pg[0] = PK_MAP;
    wr64(p, m.nid);
    wr32(p + 4, MAGIC_MAP);
    wr16(p + 8, m.state);
    wr16(p + 10, m.root);
    wr16(p + 12, m.acc);
    wr16(p + 14, m.batch_n);
    wr16(p + 16, m.c_hint);
    wr32(p + 20, m.next_vers);
    for (u32 i = 0; i < CAND_CAP; ++i) wr16(p + 24 + 2 * i, m.cand[i]);
    for (u32 i = 0; i < 64; ++i) wr16(p + 24 + 2 * CAND_CAP + 2 * i, m.ring[i]);
}

// ------------------------------------------------------------------ 锚
struct Anchor { u64 self; u16 hdr_off; u32 group; u32 magic; };
inline void anchor_write(u8* pg, u64 self, u32 group) {
    u8* p = pg + LEAF_ANCHOR;
    wr32(p, MAGIC_ANCHOR);
    wr16(p + 4, PK_LEAF);
    wr16(p + 6, LEAF_HDR);
    wr64(p + 8, self);
    wr32(p + 16, 2);          // 布局版本
    wr32(p + 20, group);
}
inline bool anchor_ok(const u8* pg) { return rd32(pg + LEAF_ANCHOR) == MAGIC_ANCHOR; }

} // namespace xdb
