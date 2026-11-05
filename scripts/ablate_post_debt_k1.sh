#!/usr/bin/env bash
# 坑 157：Post 记债免 free_ 扫 · Soft sync 消融（k1 已 Negate 删除）
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=backup/post-debt-k1
mkdir -p "$OUT"
make -j"$(nproc)" bins 2>&1 | tail -5

cat > /tmp/ablate_post157.cpp <<'CPP'
#include "include/fcs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
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
    const bool debt_snap = argc > 1 && argv[1][0] == '1';
    const u32 N = 50000, B = 500, VSZ = 32, R = 50000, S = 2000, K = 100;
    std::vector<u64> keys;
    for (u32 i = 0; i < N; ++i) keys.push_back(mix(i * 2654435761u + 12345) & SKEY_MAX);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    const u32 n = (u32)keys.size();
    std::vector<u8> val(VSZ, 7), val2(VSZ, 3);
    const char* path = "/home/user/.fcs_ablate157.db";
    std::remove(path);
    FileDev dev(path, (PageId)((n * (VSZ + 24) + 16 * 1024 * 1024) / PAGE + 8192));
    Store s;
    if (s.open(&dev, nullptr, 2048) != RC::OK) return 1;
    s.set_tidy_policy(Store::TidyPolicy::Soft);
    s.set_soft_post_debt_snap(debt_snap);
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
    std::printf("debt_snap=%d insert=%.3f update=%.3f get=%.3f range=%.3f runs=%u space_mb=%.2f\n",
                debt_snap ? 1 : 0, ins, upd, getu, rng, s.run_count(),
                (double)s.used_pages() * PAGE / (1024 * 1024));
    return 0;
}
CPP
g++ -O2 -std=c++17 -I. /tmp/ablate_post157.cpp -o /tmp/ablate_post157 -lpthread

for arm in 0 1; do
  tag=$([ "$arm" = 0 ] && echo cheap || echo snap)
  for r in 1 2 3 4 5; do
    echo "=== $tag r$r ==="
    /tmp/ablate_post157 "$arm" | tee "$OUT/${tag}_r${r}.log"
  done
done

./build/g18 2>&1 | tee "$OUT/g18.log" | tail -20

python3 - <<'PY'
import re, statistics as st
from pathlib import Path
out = Path("backup/post-debt-k1")
def parse(tag):
    rows=[]
    for r in range(1,6):
        t=(out/f"{tag}_r{r}.log").read_text()
        m=re.search(r"insert=([\d.]+) update=([\d.]+) get=([\d.]+) range=([\d.]+)", t)
        rows.append(tuple(float(x) for x in m.groups()))
    return rows
def med(rows,i): return st.median(r[i] for r in rows)
cheap, snap = parse("cheap"), parse("snap")
labs=["insert","update","get","range"]
lines=["# VERDICT_POST_DEBT（坑 157）", "",
       "Soft sync Post 50k · 中位 µs/op · 5 轮 · k1 已删（Negate）", "",
       "| metric | cheap | snap | cheap/snap |",
       "|--------|------:|-----:|-----------:|"]
for i,lab in enumerate(labs):
    a,b=med(cheap,i),med(snap,i)
    lines.append(f"| {lab} | {a:.3f} | {b:.3f} | {a/b:.2f}× |")
ins_r=med(cheap,0)/med(snap,0)
upd_r=med(cheap,1)/med(snap,1)
get_r=med(cheap,2)/med(snap,2)
rng_r=med(cheap,3)/med(snap,3)
regress=any(x>1.05 for x in (upd_r,get_r,rng_r))
win=ins_r<=0.97
neutral=all(x<=1.02 for x in (ins_r,upd_r,get_r,rng_r))
if win and not regress: verd="KEEP cheap Post debt (default)"
elif neutral: verd="KEEP_NEUTRAL cheap Post debt"
else: verd="NEGATE → default soft_post_debt_snap_=true 或删 cheap 路径"
lines += ["", f"insert cheap/snap={ins_r:.3f}× update={upd_r:.3f}× get={get_r:.3f}× range={rng_r:.3f}×",
          "", f"## VERDICT: **{verd}**", "",
          "k1 knife: NEGATE（3 轮 range 无胜且 update 回归）→ 代码已删", "",
          "复现: `bash scripts/ablate_post_debt_k1.sh`", ""]
md="\n".join(lines)
(out/"VERDICT_POST_DEBT_K1.md").write_text(md)
print(md)
PY
echo "Wrote $OUT/VERDICT_POST_DEBT_K1.md"
