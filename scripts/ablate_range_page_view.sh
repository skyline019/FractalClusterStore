#!/usr/bin/env bash
# 坑 160：range 页视图免 PAGE memcpy · Soft sync 消融
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/range-page-view
mkdir -p "$OUT"
make -j"$(nproc)" bins 2>&1 | tail -5

cat > /tmp/ablate_rpv160.cpp <<'CPP'
#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <vector>
using namespace fcs;
static u64 mix(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33; return x;
}
static double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(steady_clock::now().time_since_epoch()).count();
}
static double range_us(Store& s, const std::vector<u64>& keys, u32 S, u32 K) {
    const u32 n = (u32)keys.size();
    double t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], cnt = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++cnt; return true; }, K);
    }
    return (now_us() - t0) / (double)S;
}
int main(int argc, char** argv) {
    // argv[1]=0 → full ablate off arm only; default = same-DB xab
    const bool xab = !(argc > 1 && argv[1][0] == 'f');
    const u32 N = 50000, B = 500, VSZ = 32, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7);
    const char* path = "/home/user/.fcs_rpv160.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    s.open(&dev, nullptr, 2048);
    s.set_tidy_policy(Store::TidyPolicy::Soft);  // enables page_view via configure_deploy
    for (u32 i = 0; i < n; ++i) {
        s.put(keys[i], val.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    (void)s.soft_cons_repay(64);
    (void)range_us(s, keys, S, K);  // warmup
    if (xab) {
        double on[5], off[5];
        for (int r = 0; r < 5; ++r) {
            s.set_range_page_view(true);
            on[r] = range_us(s, keys, S, K);
            s.set_range_page_view(false);
            off[r] = range_us(s, keys, S, K);
            std::printf("r%d on=%.3f off=%.3f on/off=%.3f\n", r + 1, on[r], off[r], on[r] / off[r]);
        }
        auto med = [](double* a) {
            double b[5] = {a[0], a[1], a[2], a[3], a[4]};
            for (int i = 0; i < 5; ++i)
                for (int j = i + 1; j < 5; ++j)
                    if (b[j] < b[i]) { double t = b[i]; b[i] = b[j]; b[j] = t; }
            return b[2];
        };
        double mo = med(on), mf = med(off);
        std::printf("median on=%.3f off=%.3f on/off=%.3f\n", mo, mf, mo / mf);
    }
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_rpv160.cpp -o /tmp/ablate_rpv160 -lpthread

echo "=== xab pass1 ==="
/tmp/ablate_rpv160 | tee "$OUT/xab1.log"
echo "=== xab pass2 ==="
/tmp/ablate_rpv160 | tee "$OUT/xab2.log"

./build/g18 2>&1 | tee "$OUT/g18.log" | tail -25

python3 - <<'PY'
import re
from pathlib import Path
out = Path("backup/range-page-view")
ratios = []
for p in (out/"xab1.log", out/"xab2.log"):
    m = re.search(r"median on=([\d.]+) off=([\d.]+) on/off=([\d.]+)", p.read_text())
    ratios.append(float(m.group(3)))
    print(p.name, m.group(0))
avg = sum(ratios)/len(ratios)
verd = "KEEP Soft range_page_view" if avg <= 0.97 else (
    "KEEP_NEUTRAL" if avg <= 1.02 else "NEGATE → configure_deploy 勿开 page_view")
md = f"""# VERDICT_RANGE_PAGE_VIEW（坑 160）

同库交叉 · Soft Post repay 后 range · 两趟中位 on/off

| pass | on/off |
|------|-------:|
| 1 | {ratios[0]:.3f}× |
| 2 | {ratios[1]:.3f}× |
| avg | {avg:.3f}× |

## VERDICT: **{verd}**

约束：`select`/`scan` 回调内勿 `get`/`page`（钉 pin）。Soft `configure_deploy` 默认开。

复现: `bash scripts/ablate_range_page_view.sh`
"""
(out/"VERDICT_RANGE_PAGE_VIEW.md").write_text(md)
print(md)
PY
echo "Wrote $OUT/VERDICT_RANGE_PAGE_VIEW.md"
