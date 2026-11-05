#!/usr/bin/env bash
# 坑 158：range open 快路径（空 mem + 单段 prime）消融
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/range-open-fast
mkdir -p "$OUT"
make -j"$(nproc)" bins 2>&1 | tail -5

cat > /tmp/ablate_rof158.cpp <<'CPP'
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
int main(int argc, char** argv) {
    const bool fast = !(argc > 1 && argv[1][0] == '0');
    const u32 N = 50000, B = 500, VSZ = 32, R = 50000, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7), val2(VSZ, 3);
    const char* path = "/home/user/.fcs_rof158.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.set_range_open_fast(fast);
    double t0 = now_us();
    for (u32 i = 0; i < n; ++i) {
        if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) return 2;
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double ins = (now_us() - t0) / n;
    t0 = now_us();
    for (u32 i = 0; i < n / 2; ++i) {
        s.put(keys[mix(i + 999) % n], val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double upd = (now_us() - t0) / (n / 2);
    (void)s.soft_cons_repay(64);
    t0 = now_us();
    for (u32 i = 0; i < R; ++i) {
        u8 buf[64]; u16 l = 0;
        s.get(keys[mix(i * 7 + 1) % n], buf, sizeof buf, &l);
    }
    double getu = (now_us() - t0) / R;
    t0 = now_us();
    for (u32 i = 0; i < S; ++i) {
        u64 lo = keys[mix(i * 13 + 5) % n], cnt = 0;
        s.select(lo, lo + 4096, [&](u64, const u8*, u16) { ++cnt; return true; }, K);
    }
    double rng = (now_us() - t0) / (double)S;
    std::printf("fast=%d insert=%.3f update=%.3f get=%.3f range=%.3f runs=%u cells=%u\n",
                fast ? 1 : 0, ins, upd, getu, rng, s.run_count(), s.cell_count());
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_rof158.cpp -o /tmp/ablate_rof158 -lpthread

for arm in 1 0; do
  tag=$([ "$arm" = 1 ] && echo on || echo off)
  for r in 1 2 3 4 5; do
    echo "=== $tag r$r ==="
    /tmp/ablate_rof158 "$arm" | tee "$OUT/${tag}_r${r}.log"
  done
done

./build/g18 2>&1 | tee "$OUT/g18.log" | tail -25

python3 - <<'PY'
import re, statistics as st
from pathlib import Path
out = Path("backup/range-open-fast")
def parse(tag):
    rows=[]
    for r in range(1,6):
        t=(out/f"{tag}_r{r}.log").read_text()
        m=re.search(r"insert=([\d.]+) update=([\d.]+) get=([\d.]+) range=([\d.]+)", t)
        rows.append(tuple(float(x) for x in m.groups()))
    return rows
def med(rows,i): return st.median(r[i] for r in rows)
on, off = parse("on"), parse("off")
labs=["insert","update","get","range"]
lines=["# VERDICT_RANGE_OPEN_FAST（坑 158）", "",
       "Soft sync Post 50k · repay 后 range · 中位 µs · 5 轮", "",
       "探针：open 占 range ~83%、rows/scan≈1（lo+4096 稀疏）", "",
       "| metric | fast=on | fast=off | on/off |",
       "|--------|--------:|---------:|-------:|"]
for i,lab in enumerate(labs):
    a,b=med(on,i),med(off,i)
    lines.append(f"| {lab} | {a:.3f} | {b:.3f} | {a/b:.2f}× |")
rr=med(on,3)/med(off,3)
regress=any(med(on,i)/med(off,i)>1.05 for i in range(3))
win=rr<=0.97
neutral=all(med(on,i)/med(off,i)<=1.02 for i in range(4))
if win and not regress: verd="KEEP range_open_fast default on"
elif neutral: verd="KEEP_NEUTRAL (leave on; density OK)"
else: verd="NEGATE → default range_open_fast_=false 或删"
lines += ["", f"range on/off={rr:.3f}×", "", f"## VERDICT: **{verd}**", "",
          "复现: `bash scripts/ablate_range_open_fast.sh`", ""]
md="\n".join(lines)
(out/"VERDICT_RANGE_OPEN_FAST.md").write_text(md)
print(md)
PY
echo "Wrote $OUT/VERDICT_RANGE_OPEN_FAST.md"
