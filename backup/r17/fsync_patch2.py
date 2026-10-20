# 第 17 轮 ① 补丁 2：空票的 durable_vers、无在飞分支看 cfail_、open() 复位顺序器、cfail_ 原子
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

rep("""        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
            if (cdone_.load(std::memory_order_acquire) >= cseq_) {   // 没有在飞的提交：账已是最新
                promote_pending();
                durable_gen_ = wr_gen_;    // 没有要写的页、也没有后台已落页待入账
                return RC::OK;
            }
            // 有别的提交在飞（它的 SB 已经覆盖了我看到的全部写）：发一张**空票**排在它后面 —— commit() 返回 ⇒ 持久，
            // 所以我得等它的阶段二做完（它失败我也失败：数据是同一份）。这就是最朴素的组提交。
            t.active = true; t.seq = ++cseq_; t.my_gen = my_gen; t.err0 = err0;
            return RC::OK;
        }""",
"""        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
            if (cdone_.load(std::memory_order_acquire) >= cseq_) {   // 没有在飞的提交：账已是最新
                // 上一张票的阶段二若失败了（它的阶段三可能还在等 XLock），我看到的"最新账"并没有落盘 ⇒ 不能说 OK
                if (cfail_.load(std::memory_order_acquire)) { dirty_ = true; return RC::IO; }
                promote_pending();
                durable_gen_ = wr_gen_;    // 没有要写的页、也没有后台已落页待入账
                durable_vers_.store(std::max(durable_vers_.load(std::memory_order_relaxed), cap_vers_last_), std::memory_order_release);
                return RC::OK;
            }
            // 有别的提交在飞（它的 SB 已经覆盖了我看到的全部写）：发一张**空票**排在它后面 —— commit() 返回 ⇒ 持久，
            // 所以我得等它的阶段二做完（它失败我也失败：数据是同一份）。这就是最朴素的组提交。
            // 空票不写 SB（cap.has = false），但它在阶段三可以发布"最近一次抓的 SB 已持久"（cap_vers_last_）。
            t.active = true; t.seq = ++cseq_; t.my_gen = my_gen; t.err0 = err0; t.cap.vers = cap_vers_last_;
            return RC::OK;
        }""")

rep("""        if (t.cap.built) sb_synced_ = std::max(sb_synced_, t.cap.built);
        if (t.cap.has) durable_vers_.store(std::max(durable_vers_.load(std::memory_order_relaxed), t.cap.vers), std::memory_order_release);""",
"""        if (t.cap.built) sb_synced_ = std::max(sb_synced_, t.cap.built);
        durable_vers_.store(std::max(durable_vers_.load(std::memory_order_relaxed), t.cap.vers), std::memory_order_release);   // 空票：cap_vers_last_""")

rep("""        c.vers = vers_;
        // 坑 104：pend_delta_ 非空""",
"""        c.vers = vers_;
        cap_vers_last_ = vers_;
        // 坑 104：pend_delta_ 非空""")

rep("""        if (cfail_) rc = RC::IO;                              // 前面一张票失败""",
    """        if (cfail_.load(std::memory_order_relaxed)) rc = RC::IO;   // 前面一张票失败""")
rep("""        if (rc != RC::OK) cfail_ = true;
        cdone_.store(t.seq, std::memory_order_release);""",
"""        if (rc != RC::OK) cfail_.store(true, std::memory_order_release);
        cdone_.store(t.seq, std::memory_order_release);""")
rep("""        { std::lock_guard<std::mutex> lk(cmu_); cfail_ = false; }
        sb_synced_ = sb_built_;                    // 没有在飞的 SB 了""",
"""        { std::lock_guard<std::mutex> lk(cmu_); cfail_.store(false, std::memory_order_release); }
        sb_synced_ = sb_built_;                    // 没有在飞的 SB 了""")
rep("""    u64 cseq_ = 0;
    std::atomic<u64> cdone_{0};
    bool cfail_ = false;
    std::atomic<u64> durable_vers_{0};   // 已落盘的最新 SB 的版本（durable_vers()）""",
"""    u64 cseq_ = 0;
    std::atomic<u64> cdone_{0};
    std::atomic<bool> cfail_{false};
    std::atomic<u64> durable_vers_{0};   // 已落盘的最新 SB 的版本（durable_vers()）
    u64 cap_vers_last_ = 0;              // 最近一次抓载荷的 SB 的版本（XLock 内）：空票 / 无在飞短路用它发布 durable_vers_""")

rep("""        pend_delta_.clear();
        gpend_ = 0;
        dev_ = dev;
        cap_ = dev->max_pages ? dev->max_pages : dev->capacity();   // 缩过也能长回来""",
"""        pend_delta_.clear();
        gpend_ = 0;
        drain_inflight();                  // 第 17 轮：上一会话的提交票先做完阶段二；顺序器复位（代数不复位：pend_need_gen_ 只会比它小）
        { std::lock_guard<std::mutex> lk(cmu_); cfail_.store(false, std::memory_order_release); }
        sb_synced_ = sb_built_;
        cap_vers_last_ = 0;
        dev_ = dev;
        cap_ = dev->max_pages ? dev->max_pages : dev->capacity();   // 缩过也能长回来""")

open(P, 'w', encoding='utf-8').write(s)
print("ok")
