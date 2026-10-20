// f2：快照 / 回退（O(1) 指针回退，零数据页重写）+ 快照期间页不回收
#include "fcs.h"
#include <cstdio>
#include <map>
using namespace fcs;
static u8 arena[512 * PAGE];

static int put_i(Store& s, u64 k, u32 x) {
    u8 b[24];
    std::memcpy(b, &x, 4);
    for (int i = 4; i < 24; ++i) b[i] = (u8)(x * 7 + i);
    return s.put(k, b, 24) == RC::OK ? 0 : 1;
}
static bool get_i(Store& s, u64 k, u32 v, u32& x) {
    u8 b[64]; u16 l = 0;
    if (s.get_at(k, v, b, sizeof b, &l) != RC::OK || l != 24) return false;
    std::memcpy(&x, b, 4);
    return true;
}

int main() {
    static RamDev dev(4096);
    static Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { std::printf("open\n"); return 1; }
    const int N = 200;
    for (int i = 0; i < N; ++i) put_i(s, (u64)i * 7 + 1, 1000 + i);
    if (s.commit() != RC::OK) { std::printf("commit1\n"); return 1; }
    u32 v1 = s.last_commit();
    u64 w_before = dev.st.write_bytes;

    // 快照点
    if (s.snapshot() != RC::OK) { std::printf("snapshot\n"); return 1; }
    u64 w_snap = dev.st.write_bytes;

    // 覆盖 + 删除 + 大量新键（触发压实；快照存活 ⇒ 页延迟回收）
    for (int i = 0; i < N; ++i) put_i(s, (u64)i * 7 + 1, 2000 + i);
    for (int i = 0; i < 50; ++i) s.del((u64)(i * 7 + 1));
    for (int i = 0; i < 300; ++i) put_i(s, (u64)5000 + i, 3000 + i);
    if (s.commit() != RC::OK) { std::printf("commit2\n"); return 1; }

    // 快照视图仍可见旧值
    u32 bad_view = 0;
    for (int i = 0; i < N; ++i) {
        u32 x = 0;
        if (!get_i(s, (u64)i * 7 + 1, v1, x) || x != (u32)(1000 + i)) ++bad_view;
    }
    // 当前视图是新值
    u32 bad_now = 0;
    for (int i = 50; i < N; ++i) {
        u32 x = 0;
        if (!get_i(s, (u64)i * 7 + 1, s.open_vers(), x) || x != (u32)(2000 + i)) ++bad_now;
    }
    std::printf("snapshot view bad=%u  current view bad=%u  w_snap=%llu\n",
                bad_view, bad_now, (unsigned long long)(w_snap - w_before));

    // O(1) 回退
    u64 w_rb0 = dev.st.write_bytes;
    RC rr = s.rollback(0);
    u64 w_rb = dev.st.write_bytes - w_rb0;
    u32 bad_after = 0;
    for (int i = 0; i < N; ++i) {
        u32 x = 0;
        if (!get_i(s, (u64)i * 7 + 1, s.open_vers(), x) || x != (u32)(1000 + i)) ++bad_after;
    }
    for (int i = 0; i < 300; ++i) {
        u8 b[64]; u16 l = 0;
        if (s.get(5000 + i, b, sizeof b, &l) != RC::NOTFOUND) ++bad_after;
    }
    Report rep;
    RC vr = s.verify(&rep);
    std::printf("rollback=%s write_bytes=%llu (页数 %llu)  bad_after=%u verify=%s err=%u(%s) cells=%u\n",
                rc_str(rr), (unsigned long long)w_rb, (unsigned long long)(w_rb / PAGE), bad_after,
                rc_str(vr), rep.errors, rep.errors ? rep.first_err : "-", rep.cells);

    // 回退后可继续写
    for (int i = 0; i < 300; ++i) put_i(s, (u64)9000 + i, 4000 + i);
    if (s.commit() != RC::OK) { std::printf("commit3\n"); return 1; }
    s.forget_snapshots();

    // 重开后仍是一致状态
    RC ro = s.open(&dev, arena, 512);
    u32 bad_re = 0;
    for (int i = 0; i < N; ++i) {
        u32 x = 0;
        if (!get_i(s, (u64)i * 7 + 1, s.open_vers(), x) || x != (u32)(1000 + i)) ++bad_re;
    }
    for (int i = 0; i < 300; ++i) {
        u32 x = 0;
        if (!get_i(s, (u64)9000 + i, s.open_vers(), x) || x != (u32)(4000 + i)) ++bad_re;
    }
    RC vr2 = s.verify(&rep);
    std::printf("reopen=%s bad=%u verify=%s cells=%u runs=%u\n", rc_str(ro), bad_re, rc_str(vr2), rep.cells, rep.runs);

    bool ok = !bad_view && !bad_now && !bad_after && !bad_re && vr == RC::OK && vr2 == RC::OK && rr == RC::OK;
    std::printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
