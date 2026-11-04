#!/usr/bin/env bash
# 坑 149：Soft+Post vs Soft 基线 · SQLite（bench4）· LMDB（bench7）
# 画像：Soft sync 50k + Soft async+group+cons+shr 50k；P2 Soft-on
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/peers-post}"
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable"
  exit 1
fi

echo "# host: $(uname -a | tr '\n' ' ')" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# SoftCons::Post (pit 150) · soft=baseline Soft P2 · post=Soft+Post+soft_cons_repay" | tee -a "$OUT/META.txt"

echo "== build =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench7.cpp -o build/bench7 -llmdb
echo BUILD_OK | tee -a "$OUT/META.txt"

clean_dbs() {
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm \
        /home/user/.fcs_peers.db
  rm -rf /home/user/.lmdb_peers
}

# argv bench4: … soft=1 sp_accel=0 bf=0 skip=0 sf=1 sp=1 soft_cons
run_b4() {
  local tag="$1" sc="$2" async="$3" gm="$4" cons="$5" shr="$6" ithr="$7"
  local log="$OUT/bench4_${tag}.log"
  echo "== bench4 $tag soft_cons=$sc =="
  clean_dbs
  ./build/bench4 50000 500 32 50000 2000 100 0 "$async" "$gm" 8 0 "$cons" "$shr" "$ithr" 0 0 1 0 0 0 1 1 "$sc" \
    2>&1 | tee "$log" | tail -25
}

run_b4 soft_sync_base 0 0 0 0 0 1
run_b4 soft_sync_post 3 0 0 0 0 1
run_b4 soft_async_base 0 1 1 1 1 1
run_b4 soft_async_post 3 1 1 1 1 1

# argv bench7: … soft=1 sf=1 sp=1 soft_cons
run_b7() {
  local tag="$1" sc="$2" async="$3" gm="$4" cons="$5" shr="$6"
  local log="$OUT/bench7_${tag}.log"
  echo "== bench7 $tag soft_cons=$sc =="
  clean_dbs
  ./build/bench7 50000 500 32 50000 2000 100 "$async" "$gm" "$cons" "$shr" 1 1 1 "$sc" \
    2>&1 | tee "$log" | tail -25
}
run_b7 soft_sync_base 0 0 0 0 0
run_b7 soft_sync_post 3 0 0 0 0
run_b7 soft_async_base 0 1 1 1 1
run_b7 soft_async_post 3 1 1 1 1

python3 - <<'PY' "$OUT"
import re, sys
from pathlib import Path
out = Path(sys.argv[1])

def parse_b4(path):
    t = path.read_text(errors="replace")
    def us(phase):
        m = re.search(rf"^fcs {phase}\s+([0-9.]+)", t, re.M)
        return float(m.group(1)) if m else float("nan")
    def sq(phase):
        m = re.search(rf"^sqlite {phase}\s+([0-9.]+)", t, re.M)
        return float(m.group(1)) if m else float("nan")
    m = re.search(r"^space bytes\s+(\d+)\s+(\d+)", t, re.M)
    fcs_sp = int(m.group(1)) if m else 0
    sql_sp = int(m.group(2)) if m else 0
    return {
        "ins": us("insert"), "upd": us("update"), "get": us("get"), "rng": us("range"),
        "s_ins": sq("insert"), "s_upd": sq("update"), "s_get": sq("get"), "s_rng": sq("range"),
        "fcs_sp": fcs_sp, "sql_sp": sql_sp,
    }

def parse_b7(path):
    t = path.read_text(errors="replace")
    fm = re.search(r"# fcs done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    lm = re.search(r"# lmdb done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    if fm and lm:
        return {
            "ins": float(fm.group(1)), "upd": float(fm.group(2)), "get": float(fm.group(3)), "rng": float(fm.group(4)),
            "l_ins": float(lm.group(1)), "l_upd": float(lm.group(2)), "l_get": float(lm.group(3)), "l_rng": float(lm.group(4)),
            "fcs_sp": int(fm.group(5)), "lmdb_sp": int(lm.group(5)),
        }
    return {}

def ratio(a, b):
    if not b or a != a or b != b:
        return float("nan")
    return a / b

def fmt_r(x):
    return "—" if x != x else f"{x:.2f}×"

def fmt_us(x):
    return "—" if x != x else f"{x:.3f}"

def mb(b):
    return f"{b/1024/1024:.2f}"

cfgs = [
    ("soft_sync", "Soft sync 50k"),
    ("soft_async", "Soft async+group+cons+shr 50k"),
]

lines = [
    "# Soft + Post peers vs SQLite / LMDB（坑 149）",
    "",
    "base = Soft + P2 Soft-on，`soft_cons=Off`；post = 同配置 + `SoftCons::Post` + `soft_cons_repay`（坑 150）。",
    "比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。",
    f"复现：`bash scripts/peers_post_ab.sh` · 源 `{out}/`",
    "",
]

lines += ["## vs SQLite（bench4）", ""]
for key, title in cfgs:
    base = parse_b4(out / f"bench4_{key}_base.log")
    post = parse_b4(out / f"bench4_{key}_post.log")
    lines += [
        f"### {title}",
        "",
        "| 指标 | FCS base | FCS Post | SQLite | base/SQL | Post/SQL | Post/base |",
        "|------|--------:|---------:|-------:|---------:|---------:|----------:|",
    ]
    for lab, fk, sk in (
        ("insert µs/op", "ins", "s_ins"),
        ("update µs/op", "upd", "s_upd"),
        ("get µs/op", "get", "s_get"),
        ("range µs/scan", "rng", "s_rng"),
    ):
        b, p = base[fk], post[fk]
        sb, sp = base[sk], post[sk]
        s_show = (sb + sp) / 2 if sb == sb and sp == sp else (sb if sb == sb else sp)
        lines.append(
            f"| {lab} | {fmt_us(b)} | {fmt_us(p)} | {fmt_us(s_show)} | "
            f"{fmt_r(ratio(b, sb))} | {fmt_r(ratio(p, sp))} | {fmt_r(ratio(p, b))} |"
        )
    lines.append(
        f"| space MB | {mb(base['fcs_sp'])} | {mb(post['fcs_sp'])} | {mb(base['sql_sp'])} | "
        f"{fmt_r(ratio(base['fcs_sp'], base['sql_sp']))} | "
        f"{fmt_r(ratio(post['fcs_sp'], post['sql_sp'] or base['sql_sp']))} | "
        f"{fmt_r(ratio(post['fcs_sp'], base['fcs_sp']))} |"
    )
    lines.append("")

lines += ["## vs LMDB（bench7）", ""]
for key, title in cfgs:
    base = parse_b7(out / f"bench7_{key}_base.log")
    post = parse_b7(out / f"bench7_{key}_post.log")
    if not base or not post:
        lines += [f"### {title}", "", f"（解析失败：检查 `{key}_*.log`）", ""]
        continue
    lines += [
        f"### {title}",
        "",
        "| 指标 | FCS base | FCS Post | LMDB | base/LMDB | Post/LMDB | Post/base |",
        "|------|--------:|---------:|-----:|----------:|----------:|----------:|",
    ]
    for lab, fk, lk in (
        ("insert µs/op", "ins", "l_ins"),
        ("update µs/op", "upd", "l_upd"),
        ("get µs/op", "get", "l_get"),
        ("range µs/scan", "rng", "l_rng"),
    ):
        b, p = base[fk], post[fk]
        peer = base[lk]
        lines.append(
            f"| {lab} | {fmt_us(b)} | {fmt_us(p)} | {fmt_us(peer)} | "
            f"{fmt_r(ratio(b, peer))} | {fmt_r(ratio(p, peer))} | {fmt_r(ratio(p, b))} |"
        )
    lines.append(
        f"| space MB | {mb(base['fcs_sp'])} | {mb(post['fcs_sp'])} | {mb(base['lmdb_sp'])} | "
        f"{fmt_r(ratio(base['fcs_sp'], base['lmdb_sp']))} | "
        f"{fmt_r(ratio(post['fcs_sp'], post['lmdb_sp']))} | "
        f"{fmt_r(ratio(post['fcs_sp'], base['fcs_sp']))} |"
    )
    lines.append("")

lines += [
    "## 读法",
    "",
    "- **Post/base**：写路径应≈1.0（坑 150）；range/space 看 repay 后。",
    "- **FCS/对手**：相对 SQLite/LMDB；&lt;1 写/读更快。",
    "- 单 run vs LMDB ~2× = 坑 145 地板；**坑 151：Soft 默认已升 Post**（读前 repay）。",
    "",
]
text = "\n".join(lines)
(out / "PEERS_POST.md").write_text(text, encoding="utf-8")
print(text)
PY

echo PEERS_POST_OK out="$OUT"
