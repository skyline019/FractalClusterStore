// XDB v2 —— 公共基础：类型、返回码、配置、哈希/CRC
// 设计目标：嵌入式、无异常、无 RTTI、静态内存、小写放大。
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif

namespace xdb {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

using Nid = int32_t;          // 记录号（位置适配器）
using PageId = uint32_t;   // 32 位：单库上限 2^32 页（4 KB 页 = 16 TB），不再卡 256 MB
using Vers = uint16_t;        // 提交版本

enum : Nid { NID_NONE = -1 };

enum class RC : int {
    OK = 0,
    NOTFOUND,
    INVALID,
    FULL,       // 空间不足（缓冲/页面）：需 commit 或 settle
    NOSPACE,    // 设备满
    IO,
    CORRUPT,
    BUSY,
    RANGE,
    UNSUPPORTED,
    TIMEOUT,    // 有界等待到期（设备/后台线程迟迟不完成）：明确报错，绝不无限等
    MORE,       // 分步维护：这一步已做完并提交，但还有剩余工作，再调一次（第 11 轮 R2）
};

inline const char* rc_str(RC rc) {
    switch (rc) {
        case RC::OK: return "OK";
        case RC::MORE: return "MORE";
        case RC::NOTFOUND: return "NOTFOUND";
        case RC::INVALID: return "INVALID";
        case RC::FULL: return "FULL";
        case RC::NOSPACE: return "NOSPACE";
        case RC::IO: return "IO";
        case RC::CORRUPT: return "CORRUPT";
        case RC::BUSY: return "BUSY";
        case RC::RANGE: return "RANGE";
        case RC::UNSUPPORTED: return "UNSUPPORTED";
        case RC::TIMEOUT: return "TIMEOUT";
    }
    return "?";
}

// ------------------------------------------------------------------ 配置
enum : u32 {
    PAGE = 4096,              // 页大小（编译期常量，便于栈缓冲）
    GROUP = 32,               // 链群基数 ζ（2 的幂）
    GROUP_SLOTS = GROUP / 32, // 每群单元字数（u32 位图）
    WARM_MAX = 16,            // 热层最大条目数
    LEAFINCR = 16,            // 叶增量区（覆盖槽位 V1 等大块常驻成员）
    AUX = 8,                  // 叶附加区（内联 u ≤ 8B）
    MAX_SLOTS = 400,          // 单页槽位硬上限
    MAX_META = 64,            // 链群摘要上限
    MAX_NLU = 350,            // 常驻 u 原子数上限
#ifndef XDB_NAV_CAP
#define XDB_NAV_CAP 64
#endif
    NAV_CAP = XDB_NAV_CAP,    // 导航缓存条目数（可用 -DXDB_NAV_CAP=N 增大）
    NAV_DEPTH = 4,
    MAX_LEVEL = 10,
    SEALED_SEG_MAX = 16,      // 封存区段上限（每段 8 链）
    EQ_CAP = 32,              // 引擎级等值原子缓存（K0/高水位/代际）
    CAND_CAP = 16,            // 集合页候选数
    MAX_KEY = 64,             // 键原子上限
    MAX_VAL = 512,            // 值原子上限（batch 内）
};

struct GlobalCfg {
    u32 core_ratio = 4;          // 核区链数 = core_ratio * cand_cap
    u32 max_bf = 8;              // 每页链数上限（bf = 2^b）
    u32 ceil_pct = 70;           // 容量上限百分比（滚动水位）
    u32 lowwater_pct = 50;       // 拆卸/搬迁低水位
    u16 horizon = 8;             // 滚动代际窗口 W（决定 b）
    u16 group = GROUP;
    u32 page = PAGE;
    u16 warm_max = WARM_MAX;
};

// ------------------------------------------------------------------ 位运算
inline u32 div_up(u32 a, u32 b) { return (a + b - 1) / b; }
inline u32 ceil_mul(u32 n, u32 q) { return div_up(n, q) * q; }
inline u32 floor_mul(u32 n, u32 q) { return (n / q) * q; }
inline u32 ceil_log2(u32 v) { u32 r = 0, x = 1; while (x < v) { x <<= 1; ++r; } return r; }
inline u32 popcnt32(u32 x) { u32 c = 0; while (x) { x &= x - 1; ++c; } return c; }
inline u32 log2_floor(u32 x) { u32 r = 0; while (x > 1) { x >>= 1; ++r; } return r; }
inline u32 mask_low(u32 k) { return k >= 32 ? 0xFFFFFFFFu : ((1u << k) - 1u); }

// 模 2^k 的居中环绕差：b 是否落在 a 的 [k] 窗口内
inline bool in_window(u32 a, u32 b, u32 k) {
    u32 d = (b - a) & mask_low(32);
    if (k >= 32) return true;
    return (d & mask_low(k - 1)) == d;   // d < 2^(k-1)
}

// ------------------------------------------------------------------ 哈希
inline u32 mix32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline u64 mix64(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// 记录原子哈希（种子区分身份/排序量）
enum : u32 { SEED_SLOT = 0x51a7u, SEED_VALS = 0x7a15u, SEED_KEY = 0x6b3du, SEED_UOFF = 0x2299u, SEED_ROLL = 0x33c1u };
inline u16 atom_hash(const u8* p, u16 n, u32 seed) {
    u32 h = mix32(seed ^ (u32)n);
    for (u16 i = 0; i < n; ++i) h = mix32(h ^ p[i]) + 0x9e3779b9u;
    return (u16)(h & 0xFFFFu);
}
inline u32 roll_hash(const u8* p, u16 n, u32 seed) { return mix32(atom_hash(p, n, seed)); }

// ------------------------------------------------------------------ CRC
// CRC-32C（Castagnoli）。两条路：x86 有 SSE4.2 就走硬件指令（~10 GB/s），
// 否则用查表（~0.4 GB/s）。两条路的位序/初值/异或出口完全一致，可互相替换。
#if defined(__x86_64__) || defined(__i386__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
__attribute__((target("sse4.2")))
inline u32 crc32c_hw(const void* data, size_t n, u32 crc) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned long long c = ~(unsigned long long)crc;
    while (n >= 8) {
        unsigned long long v;
        std::memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
        p += 8; n -= 8;
    }
    while (n) { c = _mm_crc32_u8((u32)c, *p++); --n; }
    return ~(u32)c;
}
#pragma GCC diagnostic pop
inline const bool crc32c_has_hw = __builtin_cpu_supports("sse4.2") != 0;
#else
inline const bool crc32c_has_hw = false;
#endif

inline u32 crc32c(const void* data, size_t n, u32 crc = 0) {
#if defined(__x86_64__) || defined(__i386__)
    if (crc32c_has_hw) return crc32c_hw(data, n, crc);
#endif
    static u32 tab[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            tab[i] = c;
        }
        init = true;
    }
    const u8* p = (const u8*)data;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace xdb
