# 第 17 轮 ① fsync 出锁（DISCUSS15 §3）：提交分三段 + 提交顺序器 + 两个 SB 代数 + durable_vers()
import re, sys
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

# ---------- 1. commit()：阶段一持锁、阶段二锁外、阶段三再拿锁 ----------
rep("""    RC commit(bool durable = true) {
        RC rc;
        {
            WGuard g(this); if (g.busy) return RC::BUSY;
            rc = commit_locked(durable);
        }
        // R3-lite：提交点已落盘、锁已放；维护按单元做，单元之间读者/写者都能插进来。
        if (rc == RC::OK && durable) {""",
"""    RC commit(bool durable = true) {
        RC rc;
        CommitTicket t;
        {
            WGuard g(this); if (g.busy) return RC::BUSY;
            rc = commit_phase1(durable, t);          // 第 17 轮：锁内只做内存 + 发写 + 抓 SB 载荷（DISCUSS15 §3）
        }
        if (rc == RC::OK && t.active) {
            rc = commit_phase2(t);                   // 锁外：等本批页落地 → 屏障 → 写 SB → 屏障（提交者自己仍等到持久才返回）
            WGuard g(this);                          // 本线程入口已查过不持共享锁 ⇒ 这里一定拿得到
            rc = commit_phase3(t, rc);               // µs 级：发布 sb_synced_ / durable_gen_、放行挂起页
        }
        // R3-lite：提交点已落盘、锁已放；维护按单元做，单元之间读者/写者都能插进来。
        if (rc == RC::OK && durable) {""")

# ---------- 2. commit_locked → 三段 ----------
old_head = """    RC commit_locked(bool durable) {
        if (RC r = ensure_ready(); r != RC::OK) return r;
        if (!durable) {"""
new_head = """    // 第 17 轮：提交分三段（DISCUSS15 §3）。
    //   阶段一 commit_phase1（XLock 内）：ensure_ready / 组提交短路 / flush_mem（编码 + 发写）/ 清单内存更新 + 清单页 pwrite / 抓 SB 载荷
    //            ⇒ 读者从此看到新视图（run 已入账、mem_ 已清）；发一张提交票（cseq_）。
    //   阶段二 commit_phase2（XLock 外，提交顺序器 cmu_ 按票号串行）：wait_io → sync → 写 SB → sync。失败 ⇒ 粘性 cfail_（后面的票不再写 SB）。
    //   阶段三 commit_phase3（XLock 内，µs）：sb_synced_ / durable_vers_ / durable_gen_ 发布，promote_pending，R7 自动归一。
    // 持锁的调用方（maint_commit / abort / snapshot / NOSPACE 重试的中间提交点）走内联版 commit_locked：三段连着做、不放锁。
    // 语义不变：commit() 返回 ⇒ 持久；读者的可见性本来就是"可见先于持久"（未提交的 mem_ 对读者可见）。变宽的只是失败窗口：
    // 阶段二失败时内存已切到新视图、盘上还是旧提交点 ⇒ dirty_ + 按盘重建，窗口从 µs 变成一次 fsync 的 ms 级。
    struct SbCap { std::vector<u8> buf; u64 vers = 0; u64 built = 0; bool has = false; };
    struct CommitTicket { bool active = false; u64 seq = 0; u64 my_gen = 0; u64 err0 = 0; SbCap cap; };
    RC commit_locked(bool durable) {
        CommitTicket t;
        RC rc = commit_phase1(durable, t);
        if (rc != RC::OK || !t.active) return rc;
        rc = commit_phase2(t);
        return commit_phase3(t, rc);
    }
    // 锁内：等所有已发出的票做完阶段二（阶段二不需要 XLock ⇒ 不会死锁；它们的阶段三等我们放锁后再补，见 commit_phase3 的 max 语义）。
    // 内联写 SB 的路径（write_sb：rollback / shrink / 维护提交点）、reload、析构之前都要先排空：盘上 SB 的先后 = 票号先后。
    void drain_inflight() {
        if (cdone_.load(std::memory_order_acquire) >= cseq_) return;
        std::unique_lock<std::mutex> lk(cmu_);
        ccv_.wait(lk, [&] { return cdone_.load(std::memory_order_relaxed) >= cseq_; });
    }
    RC commit_phase2(CommitTicket& t) {
        std::unique_lock<std::mutex> lk(cmu_);
        ccv_.wait(lk, [&] { return cdone_.load(std::memory_order_relaxed) + 1 == t.seq; });
        RC rc = RC::OK;
        if (cfail_) rc = RC::IO;                              // 前面一张票失败：我的内存状态建立在没落盘的提交点之上 ⇒ 不写 SB，一起按盘重建
        else if (t.cap.has) {
            // 提交点必须落在数据之后（顺序性）。有界等待：设备卡住就报 TIMEOUT，而不是永远挂在条件变量上 —— "绝不挂死"的最后一道闸。
            if (!wait_io()) rc = RC::TIMEOUT;
            else if (io_err_ != t.err0) rc = RC::IO;          // 后台页写失败 ⇒ 提交点绝不越过它
            else rc = sb_flush(t.cap);
        }
        if (rc != RC::OK) cfail_ = true;
        cdone_.store(t.seq, std::memory_order_release);
        lk.unlock();
        ccv_.notify_all();
        return rc;
    }
    RC commit_phase3(CommitTicket& t, RC rc) {
        if (rc != RC::OK) { io_seen_ = io_err_; dirty_ = true; return rc; }   // 账已脏：下一次公开调用 ensure_ready() 按盘重建（有界）
        if (t.cap.built) sb_synced_ = std::max(sb_synced_, t.cap.built);
        if (t.cap.has) durable_vers_.store(std::max(durable_vers_.load(std::memory_order_relaxed), t.cap.vers), std::memory_order_release);
        durable_gen_ = std::max(durable_gen_, t.my_gen);     // 直到 my_gen 的所有写都已在此提交点内（后来的内联提交可能已经推得更远）
        u64 tp = prog_us();
        promote_pending();                                    // 提交点已落盘：挂起的页现在安全
        if (prof_on_) std::printf("[prof] %-12s %7.2f ms\\n", "promote", (double)(prog_us() - tp) / 1000.0);
        // R7（第 11 轮）：范围墓碑自动归一。墓碑一条一个清单项，几千条就把清单撑大（每次提交都要写全量快照）；
        // 阈值到了就在**本次提交点之后**顺手做一步分步归一（有界：一组 run 的归并，见 consolidate_step），
        // 若干次提交后最后一步把墓碑清掉并重写一份紧凑清单。只在无快照时做（有快照墓碑不能清）。
        // 默认关（range_auto_ = 0）：这是一个策略开关，开了每次提交多付一次提交点的代价直到清完。
        if (range_auto_ && nsnap_ == 0 && ranges_.size() >= range_auto_) {
            RC r2 = consolidate_step_impl();
            if (r2 != RC::OK && r2 != RC::MORE) { dirty_ = true; return r2; }
        }
        return RC::OK;
    }
    RC commit_phase1(bool durable, CommitTicket& t) {
        if (RC r = ensure_ready(); r != RC::OK) return r;
        if (!durable) {"""
rep(old_head, new_head)

rep("""        static const bool prof_on = std::getenv("FCS_PROF") != nullptr;
        auto prof = [](const char* tag, u64 t0) {
            if (!prof_on) return;
            std::printf("[prof] %-12s %7.2f ms\\n", tag, (double)(prog_us() - t0) / 1000.0);
        };""",
"""        auto prof = [this](const char* tag, u64 t0) {
            if (!prof_on_) return;
            std::printf("[prof] %-12s %7.2f ms\\n", tag, (double)(prog_us() - t0) / 1000.0);
        };""")

rep("""        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
            promote_pending();
            durable_gen_ = wr_gen_;        // 没有要写的页、也没有后台已落页待入账：账已是最新
            return RC::OK;
        }""",
"""        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
            if (cdone_.load(std::memory_order_acquire) >= cseq_) {   // 没有在飞的提交：账已是最新
                promote_pending();
                durable_gen_ = wr_gen_;    // 没有要写的页、也没有后台已落页待入账
                return RC::OK;
            }
            // 有别的提交在飞（它的 SB 已经覆盖了我看到的全部写）：发一张**空票**排在它后面 —— commit() 返回 ⇒ 持久，
            // 所以我得等它的阶段二做完（它失败我也失败：数据是同一份）。这就是最朴素的组提交。
            t.active = true; t.seq = ++cseq_; t.my_gen = my_gen; t.err0 = err0;
            return RC::OK;
        }""")

rep("""        // 提交点必须落在数据之后（顺序性）。有界等待：设备卡住就报 TIMEOUT，
        // 而不是永远挂在条件变量上 —— 这是\"绝不挂死\"的最后一道闸。
        if (!wait_io()) { io_seen_ = io_err_; return fail(RC::TIMEOUT, "wait_io"); }
        if (io_err_ != err0) { io_seen_ = io_err_; return fail(RC::IO, "wait_io"); }  // 后台页写失败 ⇒ 提交点绝不越过它
        rc = commit_delta(delta);
        if (rc != RC::OK) return fail(rc, "commit_delta");
        prof("commit_delta", tp); tp = prog_us();
        mem_.clear();
        mem_bytes_ = 0;
        pend_delta_.clear();
        promote_pending();                 // 提交点已落盘（write_sb 内部已同步）：挂起的页现在安全
        prof("promote", tp);
        durable_gen_ = my_gen;             // 直到 my_gen 的所有写都已在此提交点内
        // R7（第 11 轮）：范围墓碑自动归一。墓碑一条一个清单项，几千条就把清单撑大（每次提交都要写全量快照）；
        // 阈值到了就在**本次提交点之后**顺手做一步分步归一（有界：一组 run 的归并，见 consolidate_step），
        // 若干次提交后最后一步把墓碑清掉并重写一份紧凑清单。只在无快照时做（有快照墓碑不能清）。
        // 默认关（range_auto_ = 0）：这是一个策略开关，开了每次提交多付一次提交点的代价直到清完。
        if (range_auto_ && nsnap_ == 0 && ranges_.size() >= range_auto_) {
            RC r2 = consolidate_step_impl();
            if (r2 != RC::OK && r2 != RC::MORE) { dirty_ = true; return r2; }
        }
        return RC::OK;
    }
""",
"""        // 第 17 轮：wait_io / 屏障 / SB 写都挪到阶段二（锁外）。这里只做清单的内存更新 + 清单页 pwrite + 抓 SB 载荷。
        rc = commit_delta(delta, true, &t.cap);
        if (rc != RC::OK) return fail(rc, "commit_delta");
        prof("commit_delta", tp);
        mem_.clear();
        mem_bytes_ = 0;
        pend_delta_.clear();
        t.active = true; t.seq = ++cseq_; t.my_gen = my_gen; t.err0 = err0;
        return RC::OK;
    }
""")

# ---------- 3. write_sb 拆两半 ----------
rep("""    RC write_sb() {
        if (!wait_io()) return RC::TIMEOUT;
        dev_->sync();
        RC rc = write_sb_raw();
        if (rc != RC::OK) return rc;
        dev_->sync();
        // 坑 104：pend_delta_ 非空（shrink 这种只改高水位的 SB）⇒ 这次 SB 没把提交点之外维护的 DEL 落盘，
        // 不能当作\"提交点已落盘\"放行 pending_free_（否则下一次 promote_pending 把盘上仍被引用的页复用）。
        if (pend_delta_.empty()) ++sb_gen_;   // 提交点已落盘：在此之前进 pending_free_ 的页从现在起可复用
        return RC::OK;
    }
    RC write_sb_raw() {
        u32 n = (u32)std::min<size_t>(pend_.size(), INLINE_MAX);
        std::vector<u8> buf(SB_HDR + (size_t)n * MENT, 0);
        Sb sb;                                        // 永远写 v2：老库（v1 挂载）第一次写 SB 就升到 v2（清单链里的 v1 页按魔数照读）
        sb.vers_lo = (u32)vers_; sb.vers_hi = (u32)(vers_ >> 32); sb.tail = tail_;
        sb.man_first = man_first_; sb.man_last = man_last_; sb.nman = nman_;
        sb.n_inl = n;
        std::memcpy(buf.data(), &sb, SB_HDR);
        for (u32 i = 0; i < n; ++i) encode_man(buf.data() + SB_HDR + (size_t)i * MENT, pend_[i]);
        u32 chk = sb_chk(buf.data(), SB_HDR, n, MENT);
        fmt_ = FCS_FMT;
        std::memcpy(buf.data() + 12, &chk, 4);
        // 坑 83（g6）：槽位以前取 vers_ & 1。commit(false) 只前移 vers_ 不写 SB ⇒ 两次持久提交之间夹着奇数个
        // 逻辑提交时，新 SB 会写进**上一个持久提交所在的槽**：撕裂 ⇒ 两槽里只剩更老的那个 ⇒ 已持久的提交点丢失
        //（seed 2 第 1280 步：回到 258 而不是 259）。双槽协议要求的是\"每次 SB 写交替\"，与版本奇偶无关。
        u32 slot = sb_slot_ ^ 1u;
        // NOR：擦后全 1；随后把整页载荷（含提交字与 CRC）一次写入。
        // 提交的原子性由 CRC 承担：载荷没写全 ⇒ CRC 对不上；写全了就是一次完整提交。
        if (dev_->erase((PageId)slot) != RC::OK) return RC::IO;
        if (dev_->write((PageId)slot, 0, buf.data(), (u32)buf.size()) != RC::OK) return RC::IO;
        sb_slot_ = slot;
        wr_man_ += buf.size();
        tr(TR_COMMIT, (u32)vers_, tail_, man_last_, (u16)std::min<u32>(nman_, 0xFFFFu));
#ifdef FCS_TRACE
        std::fprintf(stderr, "  SBWRITE vers=%llu n_inl=%u man_last=%u nman=%u bytes=%zu\\n",
                     (unsigned long long)vers_, n, man_last_, nman_, buf.size());
#endif
        return RC::OK;
    }
""",
"""    // 内联版（持锁；rollback / shrink / mount / 维护提交点）：先排空在飞的票（盘上 SB 顺序 = 票号顺序），再抓载荷、等页、屏障、写、屏障。
    RC write_sb() {
        drain_inflight();
        SbCap c;
        sb_capture(c);
        if (!wait_io()) return RC::TIMEOUT;
        RC rc = sb_flush(c);
        if (rc != RC::OK) return rc;
        if (c.built) sb_synced_ = std::max(sb_synced_, c.built);
        durable_vers_.store(std::max(durable_vers_.load(std::memory_order_relaxed), c.vers), std::memory_order_release);
        return RC::OK;
    }
    // 第 17 轮：write_sb 拆两半。sb_capture（锁内）：抓 SB 载荷 + 统计 + 代数；sb_flush（可在锁外，按票串行）：屏障 → 擦/写槽 → 屏障。
    void sb_capture(SbCap& c) {
        u32 n = (u32)std::min<size_t>(pend_.size(), INLINE_MAX);
        c.buf.assign(SB_HDR + (size_t)n * MENT, 0);
        Sb sb;                                        // 永远写 v2：老库（v1 挂载）第一次写 SB 就升到 v2（清单链里的 v1 页按魔数照读）
        sb.vers_lo = (u32)vers_; sb.vers_hi = (u32)(vers_ >> 32); sb.tail = tail_;
        sb.man_first = man_first_; sb.man_last = man_last_; sb.nman = nman_;
        sb.n_inl = n;
        std::memcpy(c.buf.data(), &sb, SB_HDR);
        for (u32 i = 0; i < n; ++i) encode_man(c.buf.data() + SB_HDR + (size_t)i * MENT, pend_[i]);
        u32 chk = sb_chk(c.buf.data(), SB_HDR, n, MENT);
        fmt_ = FCS_FMT;
        std::memcpy(c.buf.data() + 12, &chk, 4);
        c.vers = vers_;
        // 坑 104：pend_delta_ 非空（shrink 这种只改高水位的 SB）⇒ 这次 SB 没把提交点之外维护的 DEL 落盘，
        // 不能当作\"提交点已落盘\"放行 pending_free_（否则下一次 promote_pending 把盘上仍被引用的页复用）。
        c.built = pend_delta_.empty() ? ++sb_built_ : 0;   // 代数在抓载荷时就发出（free_pages 记的是 sb_built_+1 = 下一个还没抓的 SB）
        c.has = true;
        wr_man_ += c.buf.size();
        tr(TR_COMMIT, (u32)vers_, tail_, man_last_, (u16)std::min<u32>(nman_, 0xFFFFu));
#ifdef FCS_TRACE
        std::fprintf(stderr, "  SBWRITE vers=%llu n_inl=%u man_last=%u nman=%u bytes=%zu built=%llu\\n",
                     (unsigned long long)vers_, n, man_last_, nman_, c.buf.size(), (unsigned long long)c.built);
#endif
    }
    RC sb_flush(const SbCap& c) {
        dev_->sync();                                 // 屏障：数据页 + 清单页先落
        // 坑 83（g6）：槽位以前取 vers_ & 1。commit(false) 只前移 vers_ 不写 SB ⇒ 两次持久提交之间夹着奇数个
        // 逻辑提交时，新 SB 会写进**上一个持久提交所在的槽**：撕裂 ⇒ 两槽里只剩更老的那个 ⇒ 已持久的提交点丢失
        //（seed 2 第 1280 步：回到 258 而不是 259）。双槽协议要求的是\"每次 SB 写交替\"，与版本奇偶无关。
        // 第 17 轮：槽号在**写的时候**翻（不在抓载荷时翻）：写失败槽不翻，下一次仍写同一槽，不会把唯一有效的 SB 擦掉；
        // 多张票的写由 cmu_ 串行 ⇒ sb_slot_ 只在这里（和 mount）被改。
        const u32 slot = sb_slot_ ^ 1u;
        // NOR：擦后全 1；随后把整页载荷（含提交字与 CRC）一次写入。
        // 提交的原子性由 CRC 承担：载荷没写全 ⇒ CRC 对不上；写全了就是一次完整提交。
        if (dev_->erase((PageId)slot) != RC::OK) return RC::IO;
        if (dev_->write((PageId)slot, 0, c.buf.data(), (u32)c.buf.size()) != RC::OK) return RC::IO;
        sb_slot_ = slot;
        dev_->sync();                                 // 屏障：SB 落盘
        return RC::OK;
    }
""")

# ---------- 4. commit_delta：可选抓载荷 ----------
rep("""    RC commit_delta(const std::vector<ManEnt>& delta0, bool with_ranges = true) {""",
    """    RC commit_delta(const std::vector<ManEnt>& delta0, bool with_ranges = true, SbCap* cap = nullptr) {   // cap：只抓 SB 载荷不写（commit() 阶段一）""")
rep("""            RC rc = write_man_snapshot();        // 全量快照已包含 pend_ 的效果
            if (rc != RC::OK) return rc;
            pend_.clear();
            vers_ = open_vers();
            return write_sb();
        }
        while (pend_.size() > INLINE_MAX) {
            std::vector<ManEnt> batch(pend_.begin(), pend_.begin() + INLINE_MAX);
            RC rc = write_man_page(batch);
            if (rc != RC::OK) return rc;
            pend_.erase(pend_.begin(), pend_.begin() + INLINE_MAX);
        }
        vers_ = open_vers();
        return write_sb();
    }""",
"""            RC rc = write_man_snapshot();        // 全量快照已包含 pend_ 的效果
            if (rc != RC::OK) return rc;
            pend_.clear();
            vers_ = open_vers();
            if (cap) { sb_capture(*cap); return RC::OK; }
            return write_sb();
        }
        while (pend_.size() > INLINE_MAX) {
            std::vector<ManEnt> batch(pend_.begin(), pend_.begin() + INLINE_MAX);
            RC rc = write_man_page(batch);
            if (rc != RC::OK) return rc;
            pend_.erase(pend_.begin(), pend_.begin() + INLINE_MAX);
        }
        vers_ = open_vers();
        if (cap) { sb_capture(*cap); return RC::OK; }
        return write_sb();
    }""")

# ---------- 5. 代数：sb_gen_ → sb_built_ / sb_synced_ ----------
rep("""        if (sb_gen_ < pend_need_gen_) return;""", """        if (sb_synced_ < pend_need_gen_) return;       // 第 17 轮：看"已落盘"的代数（在飞的 SB 不算）""")
rep("""        if (nsnap_ > 0 && !snap_safe) { tr(TR_FREE, p0, np, tail_, 1); deferred_.emplace_back(np, p0); def_need_gen_ = sb_gen_ + 1; return; }""",
    """        if (nsnap_ > 0 && !snap_safe) { tr(TR_FREE, p0, np, tail_, 1); deferred_.emplace_back(np, p0); def_need_gen_ = sb_built_ + 1; return; }""")
rep("""        pend_need_gen_ = sb_gen_ + 1;                                   // 释放它的 DEL 还没落盘：要等下一个 SB""",
    """        pend_need_gen_ = sb_built_ + 1;                                 // 释放它的 DEL 还没落盘：要等下一个**还没抓载荷**的 SB 落盘（第 17 轮：在飞的那个不算）""")
rep("""        if (sb_gen_ < def_need_gen_) {     // 坑 99：有延迟页的 DEL 还没过提交点 ⇒ 整表转进 pending_free_，等下一个 SB""",
    """        if (sb_synced_ < def_need_gen_) {  // 坑 99：有延迟页的 DEL 还没过提交点 ⇒ 整表转进 pending_free_，等下一个 SB""")
rep("""        pend_need_gen_ = def_need_gen_ = sb_gen_ + 1;   // 保守：都等回退后的第一个提交点""",
    """        pend_need_gen_ = def_need_gen_ = sb_built_ + 1;   // 保守：都等回退后的第一个提交点""")
rep("""        if (j.need_res && j.w && !j.w->res_np && sb_gen_ > j.seal_gen) {   // P2b：上一片封口时释放的源前缀已过提交点 ⇒ 现在预留能落进那些洞
            j.need_res = false;""",
"""        if (j.need_res && j.w && !j.w->res_np && sb_synced_ > j.seal_gen) {   // P2b：上一片封口时释放的源前缀已过提交点 ⇒ 现在预留能落进那些洞
            j.need_res = false;
            promote_pending();                                       // 第 17 轮：内联 SB（rollback/shrink）只发布代数不放行 ⇒ 这里补一次（有闸，便宜）""")
rep("""            j.need_res = true; j.seal_gen = sb_gen_;              // 预留推迟到下一个提交点之后（见 MergeJob::need_res）""",
    """            j.need_res = true; j.seal_gen = sb_built_;            // 预留推迟到下一个提交点之后（见 MergeJob::need_res）""")
rep("""        if (hw2_moves_ >= HW2_MAX || hw2_gen_ == sb_gen_ + 1) return false;""",
    """        if (hw2_moves_ >= HW2_MAX || hw2_gen_ == sb_built_ + 1) return false;""")
rep("""        hw2_gen_ = sb_gen_ + 1;                                  // 下个提交点（sb_gen_ 加一）之前不再搬第二条。只在真要搬时记：规划失败不记，""",
    """        hw2_gen_ = sb_built_ + 1;                                // 下个提交点（sb_built_ 加一）之前不再搬第二条。只在真要搬时记：规划失败不记，""")
rep("""    u64 sb_gen_ = 0;                       // 本进程成功写过的 SB 次数（单调）
    u64 pend_need_gen_ = 0;                // pending_free_ 里的页至少要等到 sb_gen_ ≥ 此值才能复用""",
"""    // 第 17 轮：SB 代数拆成两个（DISCUSS15 §2.3）。单线程时两者相等，退化成以前的单一代数。
    u64 sb_built_ = 0;                     // 本进程抓过载荷的提交点 SB 次数（XLock 内 +1；pend_delta_ 非空的"只改高水位"SB 不算，坑 104）
    u64 sb_synced_ = 0;                    // 其中已经落盘的最大代数（阶段三 / 内联 write_sb 发布）
    u64 pend_need_gen_ = 0;                // pending_free_ 里的页至少要等到 sb_synced_ ≥ 此值才能复用""")
rep("sb_gen_/pend_need_gen_ 闸", "sb_synced_/pend_need_gen_ 闸")
rep("上次搬挡路 run 时的 sb_gen_+1", "上次搬挡路 run 时的 sb_built_+1")
rep("没有 SB、sb_gen_ 不涨", "没有 SB、sb_built_ 不涨")
assert 'sb_gen_' not in s, [m.start() for m in re.finditer('sb_gen_', s)][:5]

# ---------- 6. 成员：顺序器 / durable_vers_ / prof_on_ ----------
rep("""    u64 wr_gen_ = 0;             // 每次 put/del 递增：哪些写已经\"进过 mem_\"
    u64 durable_gen_ = 0;        // 上一个成功提交点覆盖到的 wr_gen_""",
"""    u64 wr_gen_ = 0;             // 每次 put/del 递增：哪些写已经\"进过 mem_\"
    u64 durable_gen_ = 0;        // 上一个成功提交点覆盖到的 wr_gen_
    // 第 17 轮：提交顺序器。票号 cseq_ 在 XLock 内发；cdone_ 在 cmu_ 内推进（原子：drain / 阶段一在 XLock 内读它）；
    // cfail_ 粘性：一张票的阶段二失败后，后面的票不再写 SB（内存状态建立在没落盘的提交点上），reload 排空后复位。
    std::mutex cmu_;
    std::condition_variable ccv_;
    u64 cseq_ = 0;
    std::atomic<u64> cdone_{0};
    bool cfail_ = false;
    std::atomic<u64> durable_vers_{0};   // 已落盘的最新 SB 的版本（durable_vers()）
    const bool prof_on_ = std::getenv("FCS_PROF") != nullptr;""")

# ---------- 7. reload：先排空、复位顺序器 ----------
rep("""    RC reload() {
        // 重建状态前必须把在飞的后台页写完：否则 worker 会把\"旧分支的页\"
        // 写到刚被重新分配出去的位置上（就是随机坏清单项/数据错乱的来源）。
        if (!wait_io(true)) return RC::TIMEOUT;     // 这里等不到就不重建（宁可报错）""",
"""    RC reload() {
        // 第 17 轮：先把在飞的提交票排空（它们的阶段二不需要 XLock），再复位顺序器 —— 否则 cfail_ 复位后一张还没轮到的旧票会把
        // 过期的 SB 载荷写上盘。
        drain_inflight();
        { std::lock_guard<std::mutex> lk(cmu_); cfail_ = false; }
        sb_synced_ = sb_built_;                    // 没有在飞的 SB 了
        // 重建状态前必须把在飞的后台页写完：否则 worker 会把\"旧分支的页\"
        // 写到刚被重新分配出去的位置上（就是随机坏清单项/数据错乱的来源）。
        if (!wait_io(true)) return RC::TIMEOUT;     // 这里等不到就不重建（宁可报错）""")

# ---------- 8. 析构 / mount：排空、初值 ----------
rep("""    ~Store() {
        stop_maint_thread();               // mode 3：先停后台维护线程（它会拿锁、碰设备）""",
"""    ~Store() {
        drain_inflight();                  // 第 17 轮：别的线程若还有提交票在阶段二，等它写完 SB（阶段三需要本对象，调用方得自己保证不并发析构）
        stop_maint_thread();               // mode 3：先停后台维护线程（它会拿锁、碰设备）""")
rep("""    u64 last_commit() const { return vers_; }                 // P2（第 16 轮）：版本号 64 位（源码级变更：以前返回 u32）""",
"""    u64 last_commit() const { return vers_; }                 // P2（第 16 轮）：版本号 64 位（源码级变更：以前返回 u32）
    // 第 17 轮：已落盘的最新提交点版本。commit() 返回前 durable_vers() == last_commit()；另一个线程在某次 commit() 的阶段二期间
    // 读到 durable_vers() < last_commit() 就表示"看到的新视图还没持久"（读者可见性本来就先于持久，这只是给在意的应用一个自检手段）。
    u64 durable_vers() const { return durable_vers_.load(std::memory_order_acquire); }""")

# mount 成功后 durable_vers_ = vers_（挂载那一刻盘上的就是持久的）
rep("""        fmt_ = fmt_disk_ = s->fmt;                        // P2：1 = 老库（第一次写 SB 自动升到 v2）；2 = 当前格式""",
    """        fmt_ = fmt_disk_ = s->fmt;                        // P2：1 = 老库（第一次写 SB 自动升到 v2）；2 = 当前格式
        durable_vers_.store(s->vers, std::memory_order_release);""")

open(P, 'w', encoding='utf-8').write(s)
print("ok", len(s))
