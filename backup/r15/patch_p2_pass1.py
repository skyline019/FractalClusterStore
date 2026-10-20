#!/usr/bin/env python3
"""ROUND15 pass1: format constants, ManEnt v2, Run seq/ver u64, seg_key."""
from pathlib import Path

path = Path("/mnt/e/FCS/embdb/x/include/fcs.h")
text = path.read_text(encoding="utf-8")

def must_replace(old: str, new: str, label: str) -> None:
    global text
    if old not in text:
        raise SystemExit(f"MISSING: {label}")
    text = text.replace(old, new, 1)
    print("ok:", label)

must_replace(
    "enum : u32 { DHDR = 40, MHDR = 16, MENT = 40, MAN_PER_PG = 102 };",
    """enum : u32 {
    DHDR = 40, MHDR = 16,
    MENT_V1 = 40, MENT_V2 = 48,           // P2：seq/ver 改为 u64 ⇒ 条目 48B
    MENT = MENT_V2,                       // 新写默认 v2
    MAN_PER_PG_V1 = 102,
    MAN_PER_PG_V2 = (PAGE - MHDR) / MENT_V2,  // 85
    MAN_PER_PG = MAN_PER_PG_V2,
    FMT_V1 = 1, FMT_V2 = 2                // Sb.pad 槽：格式版本
};""",
    "ment constants",
)

must_replace(
    """enum : u8 { M_RUN_ADD = 1, M_RUN_DEL = 2, M_SPLIT_ADD = 3, M_SPLIT_DEL = 4, M_SEG_DEL = 5,
             M_RANGE = 6 };   // 范围墓碑：key=lo last=hi ver=版本（只活在清单里，不占键空间）""",
    """enum : u8 { M_RUN_ADD = 1, M_RUN_DEL = 2, M_SPLIT_ADD = 3, M_SPLIT_DEL = 4, M_SEG_DEL = 5,
             M_RANGE = 6,   // 范围墓碑：key=lo last=hi ver=版本（只活在清单里，不占键空间）
             M_RUN_TRIM = 7 }; // P2：源 run 前缀释放（page0=旧起点，nbytes=新 page0，npages=剩余页，key=新 first）""",
    "M_RUN_TRIM",
)

must_replace(
    'inline u64 seg_key(u32 seq, u32 rid) { return ((u64)seq << 32) | rid; }',
    """// seq 盘上/内存为 u64；现策略经重编号始终落入低 32 位可用区，seg 键仍 pack (seq32, rid)。
inline u64 seg_key(u64 seq, u32 rid) { return (seq << 32) | rid; }""",
    "seg_key",
)

must_replace(
    '    u32 ver = 0, seq = 0, refs = 0, nbytes = 0, nrec = 0;   // seq：全局单调，用于“新→旧”顺序',
    """    u64 ver = 0, seq = 0;                 // P2：u64（盘上 ManEnt 同步加宽）
    u32 refs = 0, nbytes = 0, nrec = 0;   // seq：全局单调，用于“新→旧”顺序""",
    "Run ver/seq",
)

must_replace(
    """    struct ManEnt {
        u8 op = 0;
        u8 lvl = 0;               // 借用 op 后的填充字节，不增加记录长度
        u64 key = 0, last = 0;
        u32 page0 = 0, npages = 0, ver = 0, nbytes = 0, seq = 0;
    };
    static void encode_man(u8* e, const ManEnt& m) {
        std::memset(e, 0, MENT);
        e[0] = m.op;
        e[1] = m.lvl;
        std::memcpy(e + 4, &m.ver, 4);
        std::memcpy(e + 8, &m.key, 8);
        std::memcpy(e + 16, &m.last, 8);
        std::memcpy(e + 24, &m.page0, 4);
        std::memcpy(e + 28, &m.npages, 4);
        std::memcpy(e + 32, &m.nbytes, 4);
        std::memcpy(e + 36, &m.seq, 4);
    }
    static ManEnt decode_man(const u8* e) {
        ManEnt m;
        m.op = e[0];
        m.lvl = e[1];
        std::memcpy(&m.ver, e + 4, 4);
        std::memcpy(&m.key, e + 8, 8);
        std::memcpy(&m.last, e + 16, 8);
        std::memcpy(&m.page0, e + 24, 4);
        std::memcpy(&m.npages, e + 28, 4);
        std::memcpy(&m.nbytes, e + 32, 4);
        std::memcpy(&m.seq, e + 36, 4);
        return m;
    }""",
    """    struct ManEnt {
        u8 op = 0;
        u8 lvl = 0;               // 借用 op 后的填充字节，不增加记录长度
        u64 key = 0, last = 0;
        u32 page0 = 0, npages = 0, nbytes = 0;
        u64 ver = 0, seq = 0;     // P2：u64
    };
    static u32 ment_bytes(u32 fmt) { return fmt >= FMT_V2 ? MENT_V2 : MENT_V1; }
    static u32 man_per_pg(u32 fmt) { return fmt >= FMT_V2 ? MAN_PER_PG_V2 : MAN_PER_PG_V1; }
    // v1：ver/seq 各 4B；v2：各 8B（MENT=48）。
    static void encode_man_fmt(u8* e, const ManEnt& m, u32 fmt) {
        const u32 mb = ment_bytes(fmt);
        std::memset(e, 0, mb);
        e[0] = m.op; e[1] = m.lvl;
        if (fmt >= FMT_V2) {
            std::memcpy(e + 4, &m.ver, 8);
            std::memcpy(e + 12, &m.key, 8);
            std::memcpy(e + 20, &m.last, 8);
            std::memcpy(e + 28, &m.page0, 4);
            std::memcpy(e + 32, &m.npages, 4);
            std::memcpy(e + 36, &m.nbytes, 4);
            std::memcpy(e + 40, &m.seq, 8);
        } else {
            u32 v32 = (u32)m.ver, s32 = (u32)m.seq;
            std::memcpy(e + 4, &v32, 4);
            std::memcpy(e + 8, &m.key, 8);
            std::memcpy(e + 16, &m.last, 8);
            std::memcpy(e + 24, &m.page0, 4);
            std::memcpy(e + 28, &m.npages, 4);
            std::memcpy(e + 32, &m.nbytes, 4);
            std::memcpy(e + 36, &s32, 4);
        }
    }
    static ManEnt decode_man_fmt(const u8* e, u32 fmt) {
        ManEnt m; m.op = e[0]; m.lvl = e[1];
        if (fmt >= FMT_V2) {
            std::memcpy(&m.ver, e + 4, 8);
            std::memcpy(&m.key, e + 12, 8);
            std::memcpy(&m.last, e + 20, 8);
            std::memcpy(&m.page0, e + 28, 4);
            std::memcpy(&m.npages, e + 32, 4);
            std::memcpy(&m.nbytes, e + 36, 4);
            std::memcpy(&m.seq, e + 40, 8);
        } else {
            u32 v32 = 0, s32 = 0;
            std::memcpy(&v32, e + 4, 4); m.ver = v32;
            std::memcpy(&m.key, e + 8, 8);
            std::memcpy(&m.last, e + 16, 8);
            std::memcpy(&m.page0, e + 24, 4);
            std::memcpy(&m.npages, e + 28, 4);
            std::memcpy(&m.nbytes, e + 32, 4);
            std::memcpy(&s32, e + 36, 4); m.seq = s32;
        }
        return m;
    }
    void encode_man(u8* e, const ManEnt& m) const { encode_man_fmt(e, m, disk_fmt_); }
    ManEnt decode_man(const u8* e) const { return decode_man_fmt(e, disk_fmt_); }""",
    "ManEnt encode/decode",
)

path.write_text(text, encoding="utf-8")
print("written", path)
