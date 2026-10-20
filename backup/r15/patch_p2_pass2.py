#!/usr/bin/env python3
"""ROUND15 pass2: Sb.fmt, disk_fmt_, ment-aware I/O, upgrade, M_RUN_TRIM apply."""
from pathlib import Path

path = Path("/mnt/e/FCS/embdb/x/include/fcs.h")
text = path.read_text(encoding="utf-8")

def must_replace(old: str, new: str, label: str) -> None:
    global text
    if old not in text:
        raise SystemExit(f"MISSING: {label}\n---\n{old[:200]}")
    text = text.replace(old, new, 1)
    print("ok:", label)

# Sb.pad -> fmt
must_replace(
    "        u32 n_inl = 0, pad = 0;      // 头固定 40B\n    };\n    static u32 sb_chk(const u8* buf, u32 n_inl) {\n        std::vector<u8> tmp(SB_HDR + (size_t)n_inl * MENT);\n        std::memcpy(tmp.data(), buf, tmp.size());\n        std::memset(tmp.data() + 12, 0, 4);      // chk 自身不参与\n        return crc32c(tmp.data(), (u32)tmp.size());\n    }\n    bool read_sb(u32 slot, Sb& s, std::vector<ManEnt>& inl) {\n        u8 buf[SB_HDR];\n        if (dev_->read((PageId)slot, 0, buf, SB_HDR) != RC::OK) return false;\n        std::memcpy(&s, buf, SB_HDR);\n        if (s.magic != PK_SB || s.state != COMMIT_WORD) return false;\n        if (s.n_inl > INLINE_MAX) return false;\n        std::vector<u8> all(SB_HDR + (size_t)s.n_inl * MENT);\n        std::memcpy(all.data(), buf, SB_HDR);\n        if (s.n_inl && dev_->read((PageId)slot, SB_HDR, all.data() + SB_HDR, s.n_inl * MENT) != RC::OK)\n            return false;\n        if (s.chk != sb_chk(all.data(), s.n_inl)) return false;\n        inl.clear();\n        for (u32 i = 0; i < s.n_inl; ++i) inl.push_back(decode_man(all.data() + SB_HDR + (size_t)i * MENT));\n        return true;\n    }",
    """        u32 n_inl = 0, fmt = 0;     // P2：原 pad → 格式版本（0/1=v1 MENT40，2=v2 MENT48）
    };
    static u32 sb_chk(const u8* buf, u32 n_inl, u32 fmt) {
        const u32 mb = ment_bytes(fmt);
        std::vector<u8> tmp(SB_HDR + (size_t)n_inl * mb);
        std::memcpy(tmp.data(), buf, tmp.size());
        std::memset(tmp.data() + 12, 0, 4);      // chk 自身不参与
        return crc32c(tmp.data(), (u32)tmp.size());
    }
    bool read_sb(u32 slot, Sb& s, std::vector<ManEnt>& inl) {
        u8 buf[SB_HDR];
        if (dev_->read((PageId)slot, 0, buf, SB_HDR) != RC::OK) return false;
        std::memcpy(&s, buf, SB_HDR);
        if (s.magic != PK_SB || s.state != COMMIT_WORD) return false;
        if (s.fmt > FMT_V2) return false;                 // 未知格式：当坏槽
        const u32 fmt = s.fmt >= FMT_V2 ? FMT_V2 : FMT_V1;
        const u32 mb = ment_bytes(fmt);
        if (s.n_inl > INLINE_MAX) return false;
        std::vector<u8> all(SB_HDR + (size_t)s.n_inl * mb);
        std::memcpy(all.data(), buf, SB_HDR);
        if (s.n_inl && dev_->read((PageId)slot, SB_HDR, all.data() + SB_HDR, s.n_inl * mb) != RC::OK)
            return false;
        if (s.chk != sb_chk(all.data(), s.n_inl, fmt)) return false;
        inl.clear();
        for (u32 i = 0; i < s.n_inl; ++i) inl.push_back(decode_man_fmt(all.data() + SB_HDR + (size_t)i * mb, fmt));
        return true;
    }""",
    "Sb fmt + read_sb",
)

# write_sb_raw
must_replace(
    """    RC write_sb_raw() {
        u32 n = (u32)std::min<size_t>(pend_.size(), INLINE_MAX);
        std::vector<u8> buf(SB_HDR + (size_t)n * MENT, 0);
        Sb sb;
        sb.vers = vers_; sb.tail = tail_;
        sb.man_first = man_first_; sb.man_last = man_last_; sb.nman = nman_;
        sb.n_inl = n;
        std::memcpy(buf.data(), &sb, SB_HDR);
        for (u32 i = 0; i < n; ++i) encode_man(buf.data() + SB_HDR + (size_t)i * MENT, pend_[i]);
        u32 chk = sb_chk(buf.data(), n);
        std::memcpy(buf.data() + 12, &chk, 4);""",
    """    RC write_sb_raw() {
        const u32 mb = ment_bytes(disk_fmt_);
        u32 n = (u32)std::min<size_t>(pend_.size(), INLINE_MAX);
        std::vector<u8> buf(SB_HDR + (size_t)n * mb, 0);
        Sb sb;
        sb.vers = vers_; sb.tail = tail_;
        sb.man_first = man_first_; sb.man_last = man_last_; sb.nman = nman_;
        sb.n_inl = n; sb.fmt = disk_fmt_;
        std::memcpy(buf.data(), &sb, SB_HDR);
        for (u32 i = 0; i < n; ++i) encode_man(buf.data() + SB_HDR + (size_t)i * mb, pend_[i]);
        u32 chk = sb_chk(buf.data(), n, disk_fmt_);
        std::memcpy(buf.data() + 12, &chk, 4);""",
    "write_sb_raw",
)

# mount: set disk_fmt_ from winning SB; reject fmt>2 already in read_sb
must_replace(
        """        Sb* s = (oa && ob) ? (b.vers >= a.vers ? &b : &a) : (oa ? &a : &b);
        sb_slot_ = (s == &a) ? 0 : 1;                     // 下一次 SB 写到另一槽（坑 83）
        vers_ = s->vers; tail_ = s->tail;""",
        """        Sb* s = (oa && ob) ? (b.vers >= a.vers ? &b : &a) : (oa ? &a : &b);
        sb_slot_ = (s == &a) ? 0 : 1;                     // 下一次 SB 写到另一槽（坑 83）
        disk_fmt_ = s->fmt >= FMT_V2 ? FMT_V2 : (s->fmt == 0 ? FMT_V1 : s->fmt);  // 0=老盘当 v1
        if (s->fmt > FMT_V2) return RC::CORRUPT;
        vers_ = s->vers; tail_ = s->tail;""",
    "mount disk_fmt_",
)

# empty mount init fmt=2 for new DBs
must_replace(
        """        if (!oa && !ob) {
            vers_ = 0; tail_ = 2; man_first_ = man_last_ = NIL; nman_ = 0;
            sb_slot_ = 1;                                 // 两次 write_sb 分别落到槽 0、槽 1
            clear_state();""",
        """        if (!oa && !ob) {
            vers_ = 0; tail_ = 2; man_first_ = man_last_ = NIL; nman_ = 0;
            disk_fmt_ = FMT_V2;                           // 新库直接 v2
            sb_slot_ = 1;                                 // 两次 write_sb 分别落到槽 0、槽 1
            clear_state();""",
    "new db fmt v2",
)

# replay_manifest — use disk_fmt_
must_replace(
    """        for (u32 i = 0; i < nman_ && pid != NIL; ++i) {
            const u8* p = page(pid);
            if (!p || rd32(p) != PK_MAN) return RC::CORRUPT;
            if (crc32c(p + MHDR, rd32(p + 8) * MENT) != rd32(p + 12)) return RC::CORRUPT;
            if (rd32(p + 8) > MAN_PER_PG) return RC::CORRUPT;
            chain.push_back(pid);
            pid = rd32(p + 4);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const u8* src = page(*it);
            if (!src) return RC::CORRUPT;
            u32 n = rd32(src + 8);
            // 拷贝：apply_man 里的 page() 会把这块缓冲换掉（缓存是直映的）
            std::vector<u8> buf(src + MHDR, src + MHDR + (size_t)n * MENT);
            for (u32 k = 0; k < n; ++k) apply_man(decode_man(buf.data() + (size_t)k * MENT));
        }""",
    """        const u32 mb = ment_bytes(disk_fmt_), mpg = man_per_pg(disk_fmt_);
        for (u32 i = 0; i < nman_ && pid != NIL; ++i) {
            const u8* p = page(pid);
            if (!p || rd32(p) != PK_MAN) return RC::CORRUPT;
            if (crc32c(p + MHDR, rd32(p + 8) * mb) != rd32(p + 12)) return RC::CORRUPT;
            if (rd32(p + 8) > mpg) return RC::CORRUPT;
            chain.push_back(pid);
            pid = rd32(p + 4);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const u8* src = page(*it);
            if (!src) return RC::CORRUPT;
            u32 n = rd32(src + 8);
            // 拷贝：apply_man 里的 page() 会把这块缓冲换掉（缓存是直映的）
            std::vector<u8> buf(src + MHDR, src + MHDR + (size_t)n * mb);
            for (u32 k = 0; k < n; ++k) apply_man(decode_man(buf.data() + (size_t)k * mb));
        }""",
    "replay_manifest",
)

# apply_man: add M_RUN_TRIM before M_RUN_DEL branch ends — insert after M_RUN_ADD block's closing, as else if
# Find M_RUN_DEL and prepend TRIM handler before it by replacing the else if M_RUN_DEL start
must_replace(
    """        } else if (m.op == M_RUN_DEL) {
            auto it = runs_.find(m.page0);
            if (it != runs_.end()) { delete it->second; runs_.erase(it); }""",
    """        } else if (m.op == M_RUN_TRIM) {
            // P2：前缀释放。page0=旧起点，nbytes=新 page0，npages=剩余数据页，key=新 first。
            auto it = runs_.find(m.page0);
            if (it == runs_.end()) { ++bad_man_; return; }
            RunPtr r = it->second;
            if (r->seq != m.seq || m.nbytes < m.page0 || m.npages == 0) { ++bad_man_; return; }
            const u32 old0 = r->page0, new0 = m.nbytes, drop = new0 - old0;
            if (drop >= r->npages || old0 + drop + m.npages > old0 + r->npages) {
                // 允许精确后缀：new0+npages == old0+old_npages
                if (new0 + m.npages != old0 + r->npages) { ++bad_man_; return; }
            }
            apply_trim_geom(r, new0, m.npages, m.key);
        } else if (m.op == M_RUN_DEL) {
            auto it = runs_.find(m.page0);
            if (it != runs_.end()) { delete it->second; runs_.erase(it); }""",
    "apply_man TRIM",
)

# man_ok
must_replace(
    """    static bool man_ok(const ManEnt& m) {
        if (m.op < 1 || m.op > 6) return false;
        if (m.op == M_RANGE) return m.key < m.last && m.last <= IDX_BASE;
        if (m.op == M_RUN_ADD)
            return m.npages >= 1 && m.page0 >= 2 && (u64)m.page0 + m.npages <= 0xFFFFFFFFull && m.seq != 0;
        if (m.op == M_RUN_DEL) return m.page0 >= 2;
        if (m.op == M_SEG_DEL) return m.page0 >= 2 && m.seq != 0;
        return true;
    }""",
    """    static bool man_ok(const ManEnt& m) {
        if (m.op < 1 || m.op > 7) return false;
        if (m.op == M_RANGE) return m.key < m.last && m.last <= IDX_BASE;
        if (m.op == M_RUN_ADD)
            return m.npages >= 1 && m.page0 >= 2 && (u64)m.page0 + m.npages <= 0xFFFFFFFFull && m.seq != 0;
        if (m.op == M_RUN_DEL) return m.page0 >= 2;
        if (m.op == M_RUN_TRIM)
            return m.page0 >= 2 && m.nbytes >= m.page0 && m.npages >= 1 && m.seq != 0
                && (u64)m.nbytes + m.npages <= 0xFFFFFFFFull;
        if (m.op == M_SEG_DEL) return m.page0 >= 2 && m.seq != 0;
        return true;
    }""",
    "man_ok TRIM",
)

# Fix write_man page encode to use ment_bytes — two sites around 6922
# Use replace_all for the pattern carefully

text2 = text
# append_man_page style
old_append = """        u32 n = (u32)std::min<size_t>(MAN_PER_PG, ents.size());
"""
# There may be multiple - check
if text.count("MAN_PER_PG") < 3:
    print("warn: few MAN_PER_PG left", text.count("MAN_PER_PG"))

# Broad replace MENT with ment_bytes(disk_fmt_) in write paths is risky.
# Replace specific write_man blocks.
must_replace(
    """        u32 n = (u32)std::min<size_t>(MAN_PER_PG, ents.size());
        std::memset(buf.data(), 0, PAGE);
        for (u32 i = 0; i < n; ++i) encode_man(buf.data() + MHDR + (size_t)i * MENT, ents[i]);""",
    """        const u32 mb = ment_bytes(disk_fmt_), mpg = man_per_pg(disk_fmt_);
        u32 n = (u32)std::min<size_t>(mpg, ents.size());
        std::memset(buf.data(), 0, PAGE);
        for (u32 i = 0; i < n; ++i) encode_man(buf.data() + MHDR + (size_t)i * mb, ents[i]);""",
    "append man page encode A",
)

# second occurrence may differ - try
if "n * MENT" in text:
    # replace remaining man page CRC patterns that use MENT after our first fix
    pass

# write_man_snapshot loop
must_replace(
    """            u32 n = (u32)std::min<size_t>(MAN_PER_PG, all.size() - idx);
            for (u32 i = 0; i < n; ++i) encode_man(buf + MHDR + i * MENT, all[idx + i]);""",
    """            const u32 mb = ment_bytes(disk_fmt_), mpg = man_per_pg(disk_fmt_);
            u32 n = (u32)std::min<size_t>(mpg, all.size() - idx);
            for (u32 i = 0; i < n; ++i) encode_man(buf + MHDR + i * mb, all[idx + i]);""",
    "snapshot man encode",
)

# Fix CRC lines that still use MENT after encodes - replace in write_man functions
# After first encode fix, wr32 crc still uses MENT
text = text.replace(
    "wr32(buf.data() + 12, crc32c(buf.data() + MHDR, n * MENT));\n        u32 used = MHDR + n * MENT;",
    "wr32(buf.data() + 12, crc32c(buf.data() + MHDR, n * ment_bytes(disk_fmt_)));\n        u32 used = MHDR + n * ment_bytes(disk_fmt_);",
)
text = text.replace(
    "wr32(buf + 12, crc32c(buf + MHDR, n * MENT));\n            u32 used = MHDR + n * MENT;",
    "wr32(buf + 12, crc32c(buf + MHDR, n * ment_bytes(disk_fmt_)));\n            u32 used = MHDR + n * ment_bytes(disk_fmt_);",
)
print("ok: man CRC/used ment_bytes")

# disk_fmt_ member + trims_ counter near next_seq_
must_replace(
    """    u32 next_seq_ = 0x40000000u;  // 新写入段：向上（压实/归并产物也从这里取号或沿用源的最大 seq；向下区间自 R0 起不再使用）
    u32 low_seq_ = 0x3FFFFFFFu;
    static constexpr u32 SEQ_EXHAUSTED = 0xFFFFFF00u;   // 坑 103：序号用尽线（finish_run 返回 FULL）""",
    """    u32 next_seq_ = 0x40000000u;  // 新写入段：向上（压实/归并产物也从这里取号或沿用源的最大 seq；向下区间自 R0 起不再使用）
    u32 low_seq_ = 0x3FFFFFFFu;
    static constexpr u32 SEQ_EXHAUSTED = 0xFFFFFF00u;   // 坑 103：序号用尽线（finish_run 返回 FULL）
    u32 disk_fmt_ = FMT_V2;       // P2：盘上 ManEnt 宽度；挂载时从 Sb.fmt 读入
    u64 trims_ = 0;               // P2：本打开期间 M_RUN_TRIM 次数（观测）
    bool auto_upgrade_ = true;    // 打开 v1 库后首次 durable 提交自动升到 v2""",
    "disk_fmt_ member",
)

# Public accessors near seq_renumbers
must_replace(
    "    u64 seq_renumbers() const { return renumbers_; }\n",
    """    u64 seq_renumbers() const { return renumbers_; }
    u32 disk_fmt() const { return disk_fmt_; }
    u64 trim_count() const { return trims_; }
    void set_auto_upgrade(bool v) { auto_upgrade_ = v; }
""",
    "public fmt/trim accessors",
)

path.write_text(text, encoding="utf-8")
print("pass2 written")
