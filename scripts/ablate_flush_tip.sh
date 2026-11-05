#!/usr/bin/env bash
# 坑 159：经典 flush tip 免 probe · Soft sync 消融
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/work-dist
mkdir -p "$OUT"
make -j"$(nproc)" bins 2>&1 | tail -5

cat > /tmp/ablate_ft159.cpp <<'CPP'
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
    const bool tip = !(argc > 1 && argv[1][0] == '0');
    const u32 N = 50000, B = 500, VSZ = 32, R = 20000, S = 1000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7), val2(VSZ, 3);
    const char* path = "/home/user/.fcs_ft159.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.set_flush_tip(tip);
    s.clear_flush_timers();
    double t0 = now_us();
    for (u32 i = 0; i < n; ++i) {
        if (s.put(keys[i], val.data(), (u16)VSZ) != RC::OK) return 2;
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double ins = (now_us() - t0) / n;
    u64 excl_i = s.flush_excl_us(), set_i = s.flush_settle_us();
    s.clear_flush_timers();
    t0 = now_us();
    for (u32 i = 0; i < n / 2; ++i) {
        s.put(keys[mix(i + 999) % n], val2.data(), (u16)VSZ);
        if ((i + 1) % B == 0) s.commit();
    }
    s.commit();
    double upd = (now_us() - t0) / (n / 2);
    u64 excl_u = s.flush_excl_us(), set_u = s.flush_settle_us();
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
    std::printf("tip=%d insert=%.3f update=%.3f get=%.3f range=%.3f "
                "excl_ins=%llu settle_ins=%llu excl_upd=%llu settle_upd=%llu\n",
                tip ? 1 : 0, ins, upd, getu, rng,
                (unsigned long long)excl_i, (unsigned long long)set_i,
                (unsigned long long)excl_u, (unsigned long long)set_u);
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_ft159.cpp -o /tmp/ablate_ft159 -lpthread

for arm in 1 0; do
  tag=$([ "$arm" = 1 ] && echo on || echo off)
  for r in 1 2 3 4 5; do
    echo "=== $tag r$r ==="
    /tmp/ablate_ft159 "$arm" | tee "$OUT/tip_${tag}_r${r}.log"
  done
done

./build/g18 2>&1 | tee "$OUT/g18_tip.log" | tail -25

python3 - <<'PY'
import re, statistics as st
from pathlib import Path
out = Path("backup/work-dist")
def parse(tag):
    rows=[]
    for r in range(1,6):
        t=(out/f"tip_{tag}_r{r}.log").read_text()
        m=re.search(
            r"insert=([\d.]+) update=([\d.]+) get=([\d.]+) range=([\d.]+) "
            r"excl_ins=(\d+) settle_ins=(\d+) excl_upd=(\d+) settle_upd=(\d+)", t)
        rows.append(tuple(float(x) for x in m.groups()))
    return rows
def med(rows,i): return st.median(r[i] for r in rows)
on, off = parse("on"), parse("off")
labs=["insert","update","get","range","excl_ins","settle_ins","excl_upd","settle_upd"]
lines=["# VERDICT_FLUSH_TIP（坑 159）", "",
       "Soft sync Post 50k · 中位 · 5 轮 · 经典 tip 免 probe", "",
       "| metric | tip=on | tip=off | on/off |",
       "|--------|-------:|--------:|-------:|"]
for i,lab in enumerate(labs):
    a,b=med(on,i),med(off,i)
    if i>=4: lines.append(f"| {lab} | {a:.0f} | {b:.0f} | {a/b:.2f}× |")
    else: lines.append(f"| {lab} | {a:.3f} | {b:.3f} | {a/b:.2f}× |")
si=med(on,5)/med(off,5)
su=med(on,7)/med(off,7)
ins=med(on,0)/med(off,0)
upd=med(on,1)/med(off,1)
# focus: settle_upd or settle_ins ≤0.5× OR insert/update ≤0.97 without regress >5% on get/range
regress=any(med(on,i)/med(off,i)>1.05 for i in (2,3))
win=(si<=0.50 or su<=0.50 or ins<=0.97 or upd<=0.97)
if win and not regress: verd="KEEP flush_tip default on"
elif not regress and all(med(on,i)/med(off,i)<=1.02 for i in range(4)):
    verd="KEEP_NEUTRAL"
else: verd="NEGATE → flush_tip_=false"
lines += ["", f"settle_ins on/off={si:.3f}× settle_upd={su:.3f}× insert={ins:.3f}× update={upd:.3f}×",
          "", f"## VERDICT: **{verd}**", "",
          "工作分布见 WORK_DIST.md（Soft sync 墙钟≈97% 在 commit，phase2 I/O 为主）", "",
          "复现: `bash scripts/ablate_flush_tip.sh`", ""]
md="\n".join(lines)
(out/"VERDICT_FLUSH_TIP.md").write_text(md)
print(md)
PY
echo "Wrote $OUT/VERDICT_FLUSH_TIP.md"
