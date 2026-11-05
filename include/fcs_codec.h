#pragma once
#include "common.h"
#include "layout.h"
#include <vector>
#include <memory>
#include <array>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cstdlib>

namespace fcs {

using namespace xdb;

// =============================================================================
//  M01  参数  —  容量 / 层 / MemTable / 压缩触发 / 特性宏
// =============================================================================
enum : u32 {
    BMAX        = 32 * 1024,   // B1：单元物理字节上限
#ifndef FCS_SMAX
#define FCS_SMAX 512          // 段数软上限：布隆让读不再受段数影响，这里只防内存/CPU
#endif
#ifndef FCS_LVL_DIRTY_PCT
#define FCS_LVL_DIRTY_PCT 30  // 分层合并的垃圾门槛（百分比）：够了才值得重写（第 17 轮起是运行时旋钮 set_lvl_dirty_pct 的默认值）
#endif
#define FCS_R17 1             // 第 17 轮特性标记（探针 / 测试按它选分支：MaintStats 多了 merge_src_max / merge_cur_max，有 durable_vers()）
#ifndef FCS_LVL_CLIFF
#define FCS_LVL_CLIFF 4       // 第 17 轮 ②：(b) 规则的"悬崖"——一层攒到 cliff·lvl_t 条（家族）才做无垃圾的批量合并（老值 4，硬编码）
#endif
#ifndef FCS_LVL_BIG_PAGES
#define FCS_LVL_BIG_PAGES 1024   // 第 17 轮 ②：(d) 规则里"大前缀"的页数门槛：≥ 此值的前缀按 lvl_dirty_big_pct_ 触发（默认 50 = 与小前缀相同）
#endif
#ifndef FCS_LVL_DIRTY_BIG_PCT
#define FCS_LVL_DIRTY_BIG_PCT 50   // 默认 50 = 老行为。第 17 轮 ② 实测 30 更差（layout 200k 峰值 1.98×→2.38×、2M 写 +14%，见 ROUND17 §2.3）
#endif
#ifndef FCS_GC_RATIO
#define FCS_GC_RATIO 2       // 压实“性价比”门槛：dead ≥ ratio × live 才重写
#endif
#ifndef FCS_RMETA_MIN_PAGES
#define FCS_RMETA_MIN_PAGES 16  // 小于这么多页的 run 不写元数据页（挂载时直接扫这几页）
#endif
#ifndef FCS_STREAM_SLACK_DIV
#define FCS_STREAM_SLACK_DIV 8  // 流式产物预留的余量 = 源页数 / 此值（0 = 不留余量；产物超出预留会走搬家路径，见 begin_stream）
#endif
#ifndef FCS_PART_FLOOR_PAGES
#define FCS_PART_FLOOR_PAGES 64            // P2b：按洞挑片时的最小片（页）；护洞门槛默认同此
#endif
#ifndef FCS_PROTECT_MERGE_HOLES
#define FCS_PROTECT_MERGE_HOLES 0          // 0 = 关（老行为）；configure_deploy / Soft 会打开
#endif
#ifndef FCS_RANGE_FAMILY_CHAIN
#define FCS_RANGE_FAMILY_CHAIN 1           // 坑 145：Cursor::prime 按 seq 串家族链（0 = 消融）
#endif
#ifndef FCS_PART_FRAG
#define FCS_PART_FRAG 1                    // 养洞合并 A：碎片感知切片（默认开）
#endif
#ifndef FCS_PART_FRAG_FLOOR
#define FCS_PART_FRAG_FLOOR 16             // 碎片态允许的最小片（页）；仍须 part_cap(p) ≤ 最大洞
#endif
#ifndef FCS_PART_STARVE_FLOOR
#define FCS_PART_STARVE_FLOOR 32           // 坑 139：洞饿紧切下限（>frag_floor）——16 片过碎写税大
#endif
#ifndef FCS_PART_STARVE_MAX_SLICES
#define FCS_PART_STARVE_MAX_SLICES 12      // 坑 139：洞饿最多切成几片（防 run 暴涨 / β 连发 commit_delta）
#endif
#ifndef FCS_PART_STARVE_MAX_PROMOTES
#define FCS_PART_STARVE_MAX_PROMOTES 1     // 坑 139：每作业 β 插提交次数（消融：1 保峰≈−8%+税；4 峰近平）
#endif
#ifndef FCS_PART_STARVE_PROM_MIN_PEND
#define FCS_PART_STARVE_PROM_MIN_PEND 64  // 坑 140：β 仅当 pending_free≥此页（矩阵胜者；0=不门槛）
#endif
#ifndef FCS_PART_COALESCE
#define FCS_PART_COALESCE 0                // 养洞合并 B：默认关（layout 实测抬峰，见 ROUND18 §6）
#endif
#ifndef FCS_PART_COALESCE_MAX
#define FCS_PART_COALESCE_MAX 4            // 每个合并作业最多搬开几条挡路 run
#endif
#ifndef FCS_SPACE_ACCEL_PCT
#define FCS_SPACE_ACCEL_PCT 85             // 近满加速推荐实验值；Harness 消融无峰增益 ⇒ Soft 默认关（坑 125）
#endif
    SMAX        = FCS_SMAX,    // B2：单元段数上限（读放大上界）
    MAX_SNAP    = 8,
    MAN_SNAP_AT = 64,          // 清单链页数上限 → 写全量清单
    // P2（第 16 轮）格式 v2：超级块头 48 B（版本号 64 位 + 格式号）、清单项 48 B（ver/seq 64 位 + flags）、内联上限 80 条。
    // v1 的常量只给挂载时的 v1 解码器用（老库自动升级：第一次写 SB 就是 v2，清单页按自身魔数分辨 v1/v2，见 replay_manifest）。
    FCS_FMT    = 2,
    SB_HDR     = 48,           // 超级块头（其后紧跟本次提交的内联增量）
    INLINE_MAX = 80,           // 单页可内联的增量条数上限：(4096 − 48) / 48 = 84
    SB_HDR_V1  = 40,
    INLINE_MAX_V1 = 32,
};

enum : u32 { PK_SB = 0x42534346u, PK_DATA = 0x44415441u, PK_MAN = 0x4D414E43u,
              PK_RMETA = 0x4154454Du,              // 'META'：run 元数据页（紧跟在该 run 数据页之后）
              PK_SB2 = 0x32534346u, PK_MAN2 = 0x324E414Du };   // v2：'FCS2' 超级块、'MAN2' 清单页（老代码见到 'FCS2' 直接判两槽无效 ⇒ CORRUPT，干净拒绝）
enum : u32 { COMMIT_WORD = 0xA55A5AA5u };
// 页号哨兵。以前是 0xFFFF —— 和设备上限 65535 页只差一页余量，纯属巧合才没撞上。
// 现在页号是 u32，哨兵用全 1，与任何合法页号都不重叠。
enum : u32 { NIL = 0xFFFFFFFFu, NIL32 = 0xFFFFFFFFu };
// sp 的打包格式：页号 << SP_SHIFT | 页内偏移。
// 以前偏移占 16 位（页号只有 16 位）；现在页号要 32 位了，偏移只要 12 位（PAGE=4096），
// 于是打包值能表示 2^20 页 = 4 GB 的 run。**这一步必须在放宽 PageId 之前做**，
// 否则 packed>>16 会把页号截断成 65536 的余数，扫描会静默跳错页。
static_assert(PAGE <= (1u << 12), "SP_SHIFT 要能装下页内偏移");
inline constexpr u32 SP_SHIFT = 12, SP_OFF_MASK = (1u << SP_SHIFT) - 1;
// sp 里存的是**run 内页号**，20 位 ⇒ 单条 run 最多 2^20 页 = 4 GB。
// 超过之后 sp 只是"不再新增条目"（它本来就是个提示），probe 会从最后一个条目线性往后扫，
// 还是对的，只是慢。见 DESIGN3 §9。
inline constexpr u32 SP_MAX_PAGES = 1u << (32 - SP_SHIFT);
inline u32 sp_pack(u32 page, u32 off) { return (page << SP_SHIFT) | (off & SP_OFF_MASK); }
inline u32 sp_page(u32 packed) { return packed >> SP_SHIFT; }
inline u32 sp_off(u32 packed) { return packed & SP_OFF_MASK; }
enum : u32 { DHDR = 40, MHDR = 16, MENT = 48, MAN_PER_PG = 85, MENT_V1 = 40, MAN_PER_PG_V1 = 102 };   // v2 清单项 48 B：(4096 − 16) / 48 = 85

// 键空间：最高位为 0 = 用户主键；为 1 = 二级索引条目。
// 索引键 = 1 | idx(7bit) | skey(56bit)，索引值 = 主键(8B)。
// 这样索引与主键活在同一个 store、同一次提交里 ⇒ 索引维护天然原子。
inline constexpr u64 IDX_BASE = 0x8000000000000000ull;
// 范围墓碑保留键空间（2^62 起）。用户 select 的上界会被夹到这里，
// 所以墓碑记录永远不会出现在用户扫描里。
inline constexpr u64 RT_BASE = 0x4000000000000000ull;
// 超过这个长度的值走溢出页。留足空间给页头/前缀/单页最大记录，
// 免得一条大记录把整页撑爆（单条必须能放进一页）。
inline constexpr u16 INLINE_VAL_MAX = 3000;
inline constexpr u64 SKEY_MAX = 0x00FFFFFFFFFFFFFFull;
inline constexpr u32 IDX_MAX  = 128;
inline u64 idx_enc(u32 idx, u64 skey) {
    return IDX_BASE | ((u64)(idx & 0x7Fu) << 56) | (skey & SKEY_MAX);
}

enum : u8 { M_RUN_ADD = 1, M_RUN_DEL = 2, M_SPLIT_ADD = 3, M_SPLIT_DEL = 4, M_SEG_DEL = 5,
             M_RANGE = 6,     // 范围墓碑：key=lo last=hi ver=版本（只活在清单里，不占键空间）
             M_RUN_TRIM = 7,  // v2（P2b）：源 run 前缀释放：page0=rid npages=新的起始页偏移（run 内，0 = 只推进 first；第 0 页永远留作身份锚）key=trim 后的首键
             M_RUN_HOLE = 8   // v2（P2c，预留 op 号，本轮不实现）：run 内部释放的页区间
};

// =============================================================================
//  M02  编解码  —  记录头 · 页内前缀 · CRC
// =============================================================================
// 记录头 = 键 8B + 长度 2B（共 10B，2 字节对齐）。
//   · 版本不再逐条存：一条 run 只由一次提交（或一次压实）产生 ⇒ 版本是 run 的属性
//   · 逐条 CRC 取消：页 CRC 已覆盖 [12, used) 的全部记录字节，逐条校验是重复劳动
// 于是 16B 的头部瘦到 10B —— 小值场景的写放大直接降 ~20%。
// 溢出值：len 字段写 OVFL_MARK，值区变成 16 B 描述符 {pg0, npages, total, crc}。
// 这样**页内遍历/前缀压缩/合并全都不用改**（rec_size 仍然给出正确的页上字节数），
// 只有"真正取到值"的那几个入口需要多一步：把链读进缓冲。
enum : u16 { OVFL_MARK = 0xFFFFu };
inline constexpr u32 OV_HDR = 16;                  // 溢出页头：'OVFL'|next|off|len
inline constexpr u32 OV_CAP = PAGE - OV_HDR;       // 每页有效载荷
inline u32 rec_size(u16 len) {
    return 10u + (len == OVFL_MARK ? 16u : ((len + 1u) & ~1u));
}
inline u64 prog_us() {
    return (u64)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- 页内前缀编码（v3.1 记录格式） ----------------------------------------
// 页头字节 36 = pfx（本页首末键的公共“高位字节”数，0..7），字节 37 = fmt（1 = 启用）。
// 记录 = [键后缀 8-pfx B][len 2B][值]；键 = 页首键的高 pfx 字节 ‖ 后缀。
// 有序键（自增/时间戳）一页 100 条能共享 4~7 B/条；随机键 pfx=0，格式与旧版逐字节相同。
// 旧库不用迁移：旧页的 36/37 是 0（页缓冲零初始化）⇒ pfx=0、fmt=0 ⇒ 走原路径。
inline u8 pg_fmt(const u8* pg) { return pg[37]; }
inline u8 pg_pfx(const u8* pg) { return pg[37] ? (u8)(pg[36] & 7u) : 0u; }
inline u32 pg_rec_size(u8 pfx, u16 len) {
    return 2u + (len == OVFL_MARK ? 16u : ((len + 1u) & ~1u)) + 8u - pfx;
}
inline u64 pg_key_at(const u8* pg, u32 off) {
    u8 pfx = pg_pfx(pg);
    if (!pfx) { u64 k; std::memcpy(&k, pg + off, 8); return k; }
    u8 w = (u8)(8u - pfx);
    u64 lo = 0, hi;
    std::memcpy(&lo, pg + off, w);             // 低 w 字节是键后缀（小端）
    std::memcpy(&hi, pg + 16, 8);              // 高位来自页首键
    return lo | (hi & (~0ull << (8u * w)));
}
inline const u8* pg_val_at(const u8* pg, u32 off) { return pg + off + 2u + (8u - pg_pfx(pg)); }

struct RecHdr { u64 key; u16 len; };
inline void rd_rec(const u8* pg, u32 off, RecHdr& h) {
    u8 pfx = pg_pfx(pg), w = (u8)(8u - pfx);
    std::memcpy(&h.len, pg + off + w, 2);
    if (!pfx) { std::memcpy(&h.key, pg + off, 8); return; }
    u64 lo = 0, hi;
    std::memcpy(&lo, pg + off, w);
    std::memcpy(&hi, pg + 16, 8);
    h.key = lo | (hi & (~0ull << (8u * w)));
}

} // namespace fcs
