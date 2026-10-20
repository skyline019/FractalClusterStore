// w1：多写者 + 自动组提交。
// 4 个线程各自 put+commit；断言数据一条不差、verify 干净，
// 并且 commits_merged > 0（说明"别人已经提交了我的写"这条快路径真的走到了）。
// 多写者：4 个线程各自 put+commit，验证
//  (a) 不崩、数据全对；(b) 自动组提交把 commit 次数摊薄（commits_merged > 0）。
#include "fcs.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
using namespace fcs;
static u8 arena[512 * PAGE];
int main() {
    static RamDev dev(20000);
    Store s;
    if (s.open(&dev, arena, 512) != RC::OK) { printf("open fail\n"); return 1; }
    const int T = 4, N = 5000;
    std::atomic<int> err{0};
    std::vector<std::thread> th;
    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            u8 v[24]; std::memset(v, (u8)('A' + t), sizeof v);
            for (int i = 0; i < N; ++i) {
                u64 k = (u64)t * 100000 + i + 1;
                if (s.put(k, v, 20) != RC::OK) { ++err; return; }
                if (i % 10 == 9 && s.commit() != RC::OK) { ++err; return; }
            }
            if (s.commit() != RC::OK) ++err;
        });
    for (auto& x : th) x.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Report r; s.verify(&r);
    int bad = 0;
    for (int t = 0; t < T; ++t)
        for (int i = 0; i < N; ++i) {
            u8 b[32]; u16 l = 0;
            u64 k = (u64)t * 100000 + i + 1;
            if (s.get(k, b, sizeof b, &l) != RC::OK || l != 20 || b[0] != (u8)('A' + t)) ++bad;
        }
    printf("%d 线程 × %d 条：%.0f ms  verify errors=%u  回读坏=%d  自动组提交省掉的提交=%llu  runs=%u\n",
           T, N, ms, r.errors, bad, (unsigned long long)s.commits_merged(), s.run_count());
    int fails = (err || bad || r.errors) ? 1 : 0;
    if (s.commits_merged() == 0) { std::printf("  FAIL 自动组提交没生效（commits_merged==0）\n"); ++fails; }
    std::printf(fails ? "FAILURES: %d\n" : "PASS\n", fails);
    return fails;
}
