#pragma once
#include "fcs_codec.h"

namespace fcs {

// =============================================================================
//  M03  内存表  —  MemTable：点查 / 覆盖写 / 有序吐出
// =============================================================================
// 以前是 std::map<u64, Rec{u32 ver; std::vector<u8> val; bool ovfl}>：每条记录两次堆分配
//（树节点 + 值向量）、一次 O(log n) 比较树下降，200k 条实测 0.51 µs/条；
// 而内存表的全部需求只有：点查、覆盖写、按键序吐出一次、整体清空。
// 现在：条目数组 + 开放寻址哈希（存下标）+ 一块连续值区；flush 时对 (key, idx) 排一次序。
// 条目 24 B、槽 4 B（装载 ≤ 1/2）、值按原长紧挨着放 ⇒ 没有逐条分配，清空就是三个 clear。
// 覆盖写：新值不长于旧值就原地改，否则追加到值区末尾（旧字节成垃圾；垃圾过半且 > 1 MB 时重整一次）。
class MemTable {
public:
    struct Ent {
        u64 key = 0;
        u64 ver = 0;
        u32 off = 0;          // 值在 blob_ 里的偏移
        u16 len = 0;          // 0 = 墓碑；溢出值 = 16（描述符）
        bool ovfl = false;    // 大值：值区里是 16 B 溢出描述符（真值在溢出页链里）
    };
    size_t size() const { return ents_.size(); }
    bool empty() const { return ents_.empty(); }
    const std::vector<Ent>& entries() const { return ents_; }
    Ent& ent(u32 i) { ++gen_; return ents_[i]; }     // 可写访问（调用方要改它）⇒ 视作一次变更
    u64 gen() const { return gen_; }                  // 每次变更递增：游标据此判断键序视图是否过期
    u64 epoch() const { return epoch_; }              // 每次 clear 递增：条目下标在同一 epoch 内只追加不变动
    void clear() {
        ++gen_; ++epoch_;
        ents_.clear(); blob_.clear(); garbage_ = 0;
        // 槽表按需缩回：一批 200k 之后全是 2k 的小批，不该每次都清 1 MB 的槽
        if (slots_.size() > 4096 && ents_.capacity() < slots_.size() / 8) { slots_.assign(1024, 0); return; }
        std::fill(slots_.begin(), slots_.end(), 0u);
    }
    const Ent* find(u64 key) const {
        if (ents_.empty()) return nullptr;
        const u32 mask = (u32)slots_.size() - 1;
        for (u32 i = hslot(key); ; i = (i + 1) & mask) {
            u32 s = slots_[i];
            if (!s) return nullptr;
            if (ents_[s - 1].key == key) return &ents_[s - 1];
        }
    }
    const u8* val(const Ent& e) const { return blob_.data() + e.off; }
    // 取（或建）键的条目；existed 告诉调用方是不是覆盖写
    Ent& upsert(u64 key, bool& existed) {
        if (slots_.empty()) slots_.assign(1024, 0);
        if ((ents_.size() + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        const u32 mask = (u32)slots_.size() - 1;
        u32 i = hslot(key);
        for (;; i = (i + 1) & mask) {
            u32 s = slots_[i];
            if (!s) break;
            if (ents_[s - 1].key == key) { existed = true; return ents_[s - 1]; }
        }
        existed = false;
        ++gen_;
        ents_.push_back(Ent{});
        ents_.back().key = key;
        slots_[i] = (u32)ents_.size();
        return ents_.back();
    }
    // 设值：原地覆盖 / 追加。len == 0 ⇒ 墓碑（不占值区）
    void set_val(Ent& e, const u8* p, u16 len, bool ovfl) {
        ++gen_;
        if (len <= e.len) {
            garbage_ += e.len - len;
            if (len) std::memcpy(blob_.data() + e.off, p, len);
        } else {
            garbage_ += e.len;
            e.off = (u32)blob_.size();
            blob_.insert(blob_.end(), p, p + len);
        }
        e.len = len; e.ovfl = ovfl;
        if (garbage_ > (1u << 20) && garbage_ * 2 > blob_.size()) compact();
    }
    // 键序下标（flush 用；一次排序代替树的逐条比较）
    void sorted(std::vector<u32>& ord) const {
        std::vector<std::pair<u64, u32>> kv;
        kv.reserve(ents_.size());
        for (u32 i = 0; i < ents_.size(); ++i) kv.emplace_back(ents_[i].key, i);
        std::sort(kv.begin(), kv.end());
        ord.resize(kv.size());
        for (size_t i = 0; i < kv.size(); ++i) ord[i] = kv[i].second;
    }
    const Ent& at(u32 i) const { return ents_[i]; }
    u64 blob_bytes() const { return blob_.size(); }
    void swap(MemTable& o) {
        ents_.swap(o.ents_);
        slots_.swap(o.slots_);
        blob_.swap(o.blob_);
        std::swap(garbage_, o.garbage_);
        std::swap(gen_, o.gen_);
        std::swap(epoch_, o.epoch_);
    }
private:
    u32 hslot(u64 key) const { return (u32)((key * 0x9E3779B97F4A7C15ull) >> 32) & ((u32)slots_.size() - 1); }
    void rehash(size_t n) {
        slots_.assign(n, 0);
        const u32 mask = (u32)n - 1;
        for (u32 k = 0; k < ents_.size(); ++k) {
            u32 i = hslot(ents_[k].key);
            while (slots_[i]) i = (i + 1) & mask;
            slots_[i] = k + 1;
        }
    }
    void compact() {
        ++gen_;
        std::vector<u8> nb;
        nb.reserve(blob_.size() - garbage_);
        for (auto& e : ents_) {
            u32 off = (u32)nb.size();
            nb.insert(nb.end(), blob_.data() + e.off, blob_.data() + e.off + e.len);
            e.off = off;
        }
        blob_.swap(nb);
        garbage_ = 0;
    }
    std::vector<Ent> ents_;
    std::vector<u32> slots_;     // 2 的幂；存 ents_ 下标 + 1（0 = 空）
    std::vector<u8> blob_;
    u64 garbage_ = 0;
    u64 gen_ = 1, epoch_ = 1;
};

// 数据页头 40B：0 kind | 4 next(不参与 CRC) | 8 chk | 12 ver
//                 16 first_key | 24 last_key | 32 nrec:u16 | 34 used:u16
inline void put_pg_hdr(u8* p, u64 ver, u64 first, u64 last, u16 nrec, u16 used, u8 pfx = 0) {
    wr32(p + 0, PK_DATA);
    wr32(p + 4, NIL);
    wr32(p + 12, (u32)ver);                 // P2（第 16 轮）：版本 64 位；页头这 4 字节只是信息位（没人读），存低 32 位，数据页格式不变
    std::memcpy(p + 16, &first, 8);
    std::memcpy(p + 24, &last, 8);
    wr16(p + 32, nrec);
    wr16(p + 34, used);
    p[36] = pfx & 7u; p[37] = 1; p[38] = 0; p[39] = 0;   // 页内前缀长度 + 格式位
    wr32(p + 8, crc32c(p + 12, (size_t)(used - 12)));
}

// =============================================================================
//  M04  布隆过滤器  —  段级假阳性过滤
// =============================================================================
//   每条段一个：把「这次查找要不要碰这个段」从 O(#段) 页读降成一次位测试。
//   10 bit/key ⇒ 假阳性 ~1%，内存 1.25 B/key。
struct Bloom {
    // 每键 16 bit、4 个探测位：误判率 ≈ 0.24%。
    // 读路径的代价 ≈ 1 页 + 段数 × 误判率，所以“段多”几乎不影响页读次数，
    // 这也是敢把 SMAX 放大的底气 —— 用一点点 RAM 换掉大量压实写。
    std::vector<u64> w;
    u32 bits = 0, nk = 0;

    void reset(u32 nkeys) {
        nk = 0;
        bits = (nkeys < 8 ? 8 : nkeys) * 16;
        w.assign(bits / 64 + 1, 0);
    }
    static void pos(u64 h, u32 bits, u32& a, u32& b, u32& c, u32& d) {
        a = (u32)(h % bits);
        b = (u32)((h >> 16) % bits);
        c = (u32)((h >> 32) % bits);
        d = (u32)((h >> 48) % bits);
    }
    void add(u64 h) {
        if (!bits) reset(8);
        u32 a, b, c, d;
        pos(h, bits, a, b, c, d);
        w[a >> 6] |= 1ull << (a & 63);
        w[b >> 6] |= 1ull << (b & 63);
        w[c >> 6] |= 1ull << (c & 63);
        w[d >> 6] |= 1ull << (d & 63);
        ++nk;
    }
    bool may(u64 h) const {
        if (!bits) return true;
        u32 a, b, c, d;
        pos(h, bits, a, b, c, d);
        return ((w[a >> 6] >> (a & 63)) & 1u) &&
               ((w[b >> 6] >> (b & 63)) & 1u) &&
               ((w[c >> 6] >> (c & 63)) & 1u) &&
               ((w[d >> 6] >> (d & 63)) & 1u);
    }
};

inline u64 khash(u64 k) {                       // 记录级哈希（布隆用）
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
}

// =============================================================================
//  M05  段 / 单元  —  Run · Seg · Cell · Report
// =============================================================================
// 稀疏块：每 SP 条记录一个「块」，记录块内首/末键 + 起始位置。
// 首末键让我们能在**不读页**的情况下证明「这个 run 在 [lo,hi) 里根本没有键」——
// 窄区间扫描的跳跃就靠它（随机键空间里，块与块之间的空洞很大）。
struct SpEnt {
    u64 first = 0, last = 0;
    u32 packed = 0;              // 页号 << SP_SHIFT | 页内偏移（见 sp_pack）
};

struct Run {
    u32 page0 = 0, npages = 0;
    u32 mnpg = 0;                 // 紧随其后的元数据页数（fences/pbytes/sp/bloom 的持久化副本）
    u32 mpage = 0;                // meta 页起始（0 = 没有）；挂载时先记账，真要用才读
    u32 novfl = 0;                // 本 run 里溢出值的条数（>0 才需要在释放/重建空闲表时扫链）
    // 本 run **自有**的溢出链（会随这条 run 一起释放）。
    // 归并时"原样接管"的链记在 ovfl_ref 里：它不是我们的，摘链时不能释放。
    std::vector<std::pair<u32, u32>> own_chains;
    std::vector<std::pair<u32, u32>> ovfl_ref;     // 接管来的链（释放时不还）
    u32 aux_pg = NIL, aux_npg = 0;                 // 溢出链清单页（`'AOVL'`），nrec_ovfl > 0 时有
    // 页上每条溢出记录的 (数据页, 描述符偏移)。纯 RAM，封页时由 RunWriter 抄过来；
    // 有它才能"零读接管"链（归并时不用把大值读出来再写一遍）。
    std::vector<std::pair<u32, u32>> ovfl_loc;    // 页上的描述符位置 (数据页, 记录偏移)
    bool loc_ready = false;                      // ovfl_loc 已经建立过（**空表也算数**）
    // 0 = 就绪，1 = 待装，2 = 正在装（P1：共享模式下多个读者可能同时第一次碰到这条 run）
    std::atomic<u8> meta_pending{0};
    u64 first = 0, last = 0;
    u64 ver = 0, seq = 0;         // P2（第 16 轮）：64 位（坑 102/103 的回绕与重编号退役）
    u32 refs = 0, nbytes = 0, nrec = 0;   // seq：全局单调，用于“新→旧”顺序
    u32 lvl = 0;                  // 层号：0 = 刚刷盘；分层合并后上升
    // 运行级存活账：该 run 里“仍是某键最新副本”的记录数/字节数。
    // 归零 ⇒ 整个 run 全是垃圾，可直接摘链释放（零写回收）。
    u64 live_bytes = 0;
    u32 live_recs = 0;
    // P2b（第 16 轮）：分片归并的源裁剪。trim ≥ 1 ⇒ 数据页 [1,trim) 已释放（第 0 页留作身份锚，内容是死数据，不再被引用）；
    // first 已推进到裁剪边界键 B（≥ fences[trim]，页 trim 可能是"死活混合页"，键 < B 的那一截靠 first/段 klo 隔离）；
    // nbytes/nrec 是裁后的值。first0/nbytes0/nrec0 = RUN_ADD / RMETA 里的原始值：ensure_meta 用原始值和 RMETA 对账，
    // 清单的 RUN_ADD 也必须写原始值（否则重放时 RMETA 对不上就会退回扫页——扫到已释放的页）。
    u32 trim = 0;
    u64 first0 = 0;
    u32 nbytes0 = 0, nrec0 = 0;
    // 归并片进行中：页号 ∈ [1, take_lim) 的记录才允许"零读接管"溢出链（这些页在本片封口时释放）；第 0 页和 ≥ take_lim 的页
    // 本片不释放 ⇒ 链仍归源 run，记录按物化拷贝。NIL = 不限（源 run 本次整条销毁）。
    u32 take_lim = NIL;
    // R6-lite：挂载时不算存活账 ⇒ live_bytes 先按 nbytes（全活，保守）；惰性补算（recount_sweep）期间把各单元的
    // 赢家字节累进 lb_acc，全部单元补算完再一次性换成精确值（层合并的脏度判断从此"照常"）。
    bool lb_unknown = false;
    u64 lb_acc = 0;
    std::vector<u64> fences;      // 每页首键
    std::vector<u32> pbytes;      // 每页有效字节
    // pbytes 的前缀和（npages+1 项）。recount 要算「本单元这些页里一共有多少记录字节」，
    // 有了它整页区间是 O(1)，只有单元边界落在的那一两页才需要真去解码。
    std::vector<u64> pcum;
    std::vector<u16> pnrec;       // 每页记录条数（页头 +32 处就有，抄一份省一次读页）
    std::vector<u32> pncum;       // pnrec 的前缀和（单选段单元的 live_recs 靠它 O(1) 算）
    // 稀疏记录索引：每 SP 条记录存 (键, 页<<16|页内偏移)，用于把“定位到某键”
    // 从“页内线性 ≤nrec 步”降到“二分 + ≤SP 步”。纯 RAM，挂载时重建。
    std::vector<SpEnt> sp;
    // 可选：该 run 的全部键（8 B/键）。打开后区间扫描可以**精确**判断
    // “这条 run 在 [lo,hi) 里有没有键”，窄区间扫描只碰真正命中的页。
    std::vector<u64> kidx;
    u32 nrec_idx = 0;             // 仅用于重建 sp 的计数
    Bloom bloom;                  // 键存在性预判（读放大 ≈ 1 页）
};
using RunPtr = Run*;

// 段 = (run, 页区间, 键域)。键域让“一个页被相邻单元共享”变得安全：
// 每段只为该单元自己那一截键负责，页里邻居的记录既不计入也不可读。
// live：本单元内由该段胜出的记录数。run->live_recs == 它在所有单元里的 live 之和，
// 因此 run 级存活账和“逐单元归并”的口径永远一致（跨单元、压实产物同 seq 都对得上）。
struct Seg {
    u32 rid = 0, p0 = 0, p1 = 0; u64 klo = 0, khi = ~0ull; u32 live = 0;
    u8 unk = 0;                                    // R6-lite（第 12 轮）：存活数未知（live 里是占位 1 条，保证 run 不被当成全死）
    // C2（第 12 轮）：seg_totals 的 memo —— (run, 单元键域) 决定的常量；键域对不上（单元切分后复制过来的段）就重算
    mutable u64 tlo = 1, thi = 0; mutable u64 tb = 0; mutable u32 tn = 0;
};

// C1（第 12 轮）：单元的段表从 std::map 换成**有序 vector**。一个单元通常只有 1 ~ 8 段，map 每段一个堆结点
//（结点 ≈ 48 B 开销 + 三次指针跳转）在 split_pass 候选扫描 / total_rec_bytes / recount / get 的 rbegin 遍历里是纯浪费。
// 接口保持 std::map 的子集（find/count/erase/emplace_hint/operator[]/lower_bound/begin/end/rbegin/rend/size/empty/swap/clear），
// 调用点一行不改。与 map 的**唯一语义差别**：往同一个单元插入/删除段会让该单元其它段的迭代器和 Seg& 失效——
// 现有调用点都是"先 find 再改/删，不跨插入持引用"（审计见 backup/r18/swept/docs-hist/DISCUSS12.md C1）；新代码别在持 Seg& 期间往同一单元插段。
// P2（第 16 轮）：段键 = (seq 64 位, rid)。以前是 (seq << 32 | rid) 装在一个 u64 里；seq 放宽到 64 位后装不下，改成结构体，
// 顺序语义不变：先按 seq（新→旧）再按 rid。
struct SegKey {
    u64 seq = 0; u32 rid = 0;
    bool operator<(const SegKey& o) const { return seq != o.seq ? seq < o.seq : rid < o.rid; }
    bool operator>(const SegKey& o) const { return o < *this; }
    bool operator<=(const SegKey& o) const { return !(o < *this); }
    bool operator>=(const SegKey& o) const { return !(*this < o); }
    bool operator==(const SegKey& o) const { return seq == o.seq && rid == o.rid; }
    bool operator!=(const SegKey& o) const { return !(*this == o); }
};
inline SegKey seg_key(u64 seq, u32 rid) { return SegKey{seq, rid}; }
inline constexpr u64 SEQ_BASE = 1ull << 62;   // 新 run 的序号从这里向上；向下区间（low_seq_）自 R0 起不再使用

class SegMap {
public:
    using value_type = std::pair<SegKey, Seg>;
    using Vec = std::vector<value_type>;
    using iterator = Vec::iterator;
    using const_iterator = Vec::const_iterator;
    using reverse_iterator = Vec::reverse_iterator;
    using const_reverse_iterator = Vec::const_reverse_iterator;
    iterator begin() { return v_.begin(); }
    iterator end() { return v_.end(); }
    const_iterator begin() const { return v_.begin(); }
    const_iterator end() const { return v_.end(); }
    reverse_iterator rbegin() { return v_.rbegin(); }
    reverse_iterator rend() { return v_.rend(); }
    const_reverse_iterator rbegin() const { return v_.rbegin(); }
    const_reverse_iterator rend() const { return v_.rend(); }
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    void clear() { v_.clear(); }
    void swap(SegMap& o) { v_.swap(o.v_); }
    iterator lower_bound(SegKey k) { return std::lower_bound(v_.begin(), v_.end(), k, [](const value_type& a, SegKey b) { return a.first < b; }); }
    const_iterator lower_bound(SegKey k) const { return std::lower_bound(v_.begin(), v_.end(), k, [](const value_type& a, SegKey b) { return a.first < b; }); }
    iterator find(SegKey k) { auto it = lower_bound(k); return it != v_.end() && it->first == k ? it : v_.end(); }
    const_iterator find(SegKey k) const { auto it = lower_bound(k); return it != v_.end() && it->first == k ? it : v_.end(); }
    size_t count(SegKey k) const { return find(k) != v_.end() ? 1 : 0; }
    Seg& operator[](SegKey k) {
        auto it = lower_bound(k);
        if (it == v_.end() || it->first != k) it = v_.insert(it, value_type(k, Seg{}));
        return it->second;
    }
    // 与 std::map::emplace_hint 同语义：键已存在则**不覆盖**、返回已有项；提示迭代器只是提示（这里忽略，二分定位）
    iterator emplace_hint(const_iterator, SegKey k, Seg sg) {
        auto it = lower_bound(k);
        if (it != v_.end() && it->first == k) return it;
        return v_.insert(it, value_type(k, std::move(sg)));
    }
    iterator erase(const_iterator it) { return v_.erase(it); }
    size_t erase(SegKey k) { auto it = find(k); if (it == v_.end()) return 0; v_.erase(it); return 1; }
private:
    Vec v_;
};

struct Cell {
    u64 lo = 0, hi = ~0ull;
    // 键 = (段版本 << 32 | run_id)：rbegin() 即“最新段”，读取可命中最新的那份
    SegMap segs;
    u64 pages = 0;                             // 段视图覆盖页数
    u64 live = 0;                              // 存活记录字节（精确；acct=false 时无意义）
    u64 dead = 0;                              // 垃圾记录字节（精确；acct=false 时无意义）
    bool acct = true;                          // R6-lite（第 12 轮）：存活账是否已算（挂载后惰性补算）
    bool lb_added = false;                     // 本单元的赢家字节是否已经累进过"挂载前 run"的 lb_acc（避免重复累加）
    // 第 14 轮：分步作业的增量账。jacc = 本单元在 MergeJob::acc 里的下标 + 1（0 = 作业还没归并到这个单元 / 没有作业）；
    // jdirty = 作业归并过本单元之后，有别的东西改过本单元的账且增量公式对不上（压实 / 切分 / 范围墓碑 / 重写了墓碑）⇒ 收尾回退到补算。
    u32 jacc = 0;
    bool jdirty = false;
    u64 bytes() const { return live + dead; }
};


struct Report {
    u32 cells = 0, runs = 0, recs = 0, segs = 0, ovfl = 0;
    u64 ovfl_pages = 0;        // 溢出链占的物理页（不在任何 run 的 [page0,page0+npages) 里）
    u64 bytes = 0, live = 0, pages = 0;
    u32 errors = 0;
    const char* first_err = "";
    u64 first_key = 0;
};


} // namespace fcs
