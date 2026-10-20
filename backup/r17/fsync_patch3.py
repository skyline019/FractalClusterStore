# 第 17 轮 ① 补丁 3：need_sb_ —— 阶段二失败后内存账里有没落盘的提交点，下一次 commit() 哪怕没有新写也必须再写一个 SB
P = '/home/user/embdb/x/include/fcs.h'
s = open(P, encoding='utf-8').read()
def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    assert n == cnt, (n, cnt, old[:100])
    s = s.replace(old, new)

rep("""        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty()) {
            if (cdone_.load(std::memory_order_acquire) >= cseq_) {   // 没有在飞的提交：账已是最新""",
"""        // need_sb_：上一次抓了载荷的 SB 没落盘（阶段二失败 ⇒ 按盘重建后内存账仍含那次的 run / 清单页，而盘上 SB 没有）
        // ⇒ 哪怕本次没有新写也要再抓一个 SB 并写下去，否则"没东西可写"分支会直接返回 OK，重开后那批数据就没了（g17 t_phase2_fail）。
        if (delta.empty() && pend_delta_.empty() && pend_ranges_.empty() && !need_sb_) {
            if (cdone_.load(std::memory_order_acquire) >= cseq_) {   // 没有在飞的提交：账已是最新""")

rep("""    RC commit_phase3(CommitTicket& t, RC rc) {
        if (rc != RC::OK) { io_seen_ = io_err_; dirty_ = true; return rc; }   // 账已脏：下一次公开调用 ensure_ready() 按盘重建（有界）
        if (t.cap.built) sb_synced_ = std::max(sb_synced_, t.cap.built);""",
"""    RC commit_phase3(CommitTicket& t, RC rc) {
        if (rc != RC::OK) {                                   // 账已脏：下一次公开调用 ensure_ready() 按盘重建（有界）
            io_seen_ = io_err_; dirty_ = true;
            if (t.cap.has) need_sb_ = true;                   // 这张票抓过载荷却没落盘
            return rc;
        }
        if (t.cap.has) need_sb_ = false;
        if (t.cap.built) sb_synced_ = std::max(sb_synced_, t.cap.built);""")

rep("""        SbCap c;
        sb_capture(c);
        if (!wait_io()) return RC::TIMEOUT;
        RC rc = sb_flush(c);
        if (rc != RC::OK) return rc;
        if (c.built) sb_synced_ = std::max(sb_synced_, c.built);""",
"""        SbCap c;
        sb_capture(c);
        if (!wait_io()) { need_sb_ = true; return RC::TIMEOUT; }
        RC rc = sb_flush(c);
        if (rc != RC::OK) { need_sb_ = true; return rc; }
        need_sb_ = false;
        if (c.built) sb_synced_ = std::max(sb_synced_, c.built);""")

rep("""    u64 cap_vers_last_ = 0;              // 最近一次抓载荷的 SB 的版本（XLock 内）：空票 / 无在飞短路用它发布 durable_vers_""",
"""    u64 cap_vers_last_ = 0;              // 最近一次抓载荷的 SB 的版本（XLock 内）：空票 / 无在飞短路用它发布 durable_vers_
    bool need_sb_ = false;               // 抓过载荷的 SB 没落盘（阶段二 / 内联写失败）：下一次 commit() 必须再写 SB（XLock 内）""")

rep("""        sb_synced_ = sb_built_;
        cap_vers_last_ = 0;
        dev_ = dev;""",
"""        sb_synced_ = sb_built_;
        cap_vers_last_ = 0;
        need_sb_ = false;                  // 新挂载：盘上 SB 就是账
        dev_ = dev;""")
open(P, 'w', encoding='utf-8').write(s)
print("ok")
