#!/usr/bin/env bash
# P2 both vs Soft 默认关：FCS↔SQLite（bench4）· FCS↔LMDB（bench7）
# 画像：Soft sync 50k + Soft async+group+cons+shr 50k（与 peers-soft 对齐）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/peers-p2}"
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable (bench paths)"
  exit 1
fi

echo "# host: $(uname -a | tr '\n' ' ')" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# knife: P2 Soft-on (pit 144) · off=explicit disable · both=Soft default" | tee -a "$OUT/META.txt"

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

# Soft sync: soft=1 sp_accel=0 bf=0 skip=0 starve_fit starve_promote
# argv bench4: N B VSZ R S K kidx async gm lt am cons shr ithr kmode hwpct soft sp_accel bf bf_skip sf sp
run_b4() {
  local tag="$1" sf="$2" sp="$3"
  shift 3
  # remaining: async gm cons shr ithr (and soft already 1)
  local async="$1" gm="$2" cons="$3" shr="$4" ithr="$5"
  local log="$OUT/bench4_${tag}.log"
  echo "== bench4 $tag sf=$sf sp=$sp =="
  clean_dbs
  ./build/bench4 50000 500 32 50000 2000 100 0 "$async" "$gm" 8 0 "$cons" "$shr" "$ithr" 0 0 1 0 0 0 "$sf" "$sp" \
    2>&1 | tee "$log" | tail -20
}

# Soft async+cons: async=1 gm=1 cons=1 shr=1 ithr=1
# Soft sync:       async=0 gm=0 cons=0 shr=0 ithr=1
run_b4 soft_sync_off 0 0 0 0 0 0 1
run_b4 soft_sync_both 1 1 0 0 0 0 1
run_b4 soft_async_off 0 0 1 1 1 1 1
run_b4 soft_async_both 1 1 1 1 1 1 1

# bench7: N B VSZ R S K async gm cons shr soft [sf] [sp]
run_b7() {
  local tag="$1" sf="$2" sp="$3" async="$4" gm="$5" cons="$6" shr="$7"
  local log="$OUT/bench7_${tag}.log"
  echo "== bench7 $tag sf=$sf sp=$sp =="
  clean_dbs
  ./build/bench7 50000 500 32 50000 2000 100 "$async" "$gm" "$cons" "$shr" 1 "$sf" "$sp" \
    2>&1 | tee "$log" | tail -20
}
run_b7 soft_sync_off 0 0 0 0 0 0
run_b7 soft_sync_both 1 1 0 0 0 0
run_b7 soft_async_off 0 0 1 1 1 1
run_b7 soft_async_both 1 1 1 1 1 1

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
    # space bytes line: "space bytes         3203072      2347008"
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
    # comparison table rows: insert us/op ...
    def row(phase):
        m = re.search(rf"^{phase}\s+us/(?:op|scan)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)", t, re.M)
        if not m:
            # alternate: phase column layout
            m = re.search(rf"^{phase}\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)", t, re.M)
        if not m:
            return float("nan"), float("nan"), float("nan")
        return float(m.group(1)), float(m.group(2)), float(m.group(3))
    # Prefer # fcs done / # lmdb done
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
    if not b or a != a or b != b: return float("nan")
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
    "# P2 peers：FCS vs SQLite / LMDB（坑 144 Soft 默认开）",
    "",
    "both = Soft/`configure_deploy` 默认 P2；off = argv 显式关。",
    "比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。",
    f"复现：`bash scripts/peers_p2_ab.sh` · 源 `{out}/`",
    "",
]

# --- SQLite ---
lines += ["## vs SQLite（bench4）", ""]
for key, title in cfgs:
    off = parse_b4(out / f"bench4_{key}_off.log")
    both = parse_b4(out / f"bench4_{key}_both.log")
    lines += [
        f"### {title}",
        "",
        "| 指标 | FCS off | FCS both | SQLite | off/SQL | both/SQL | both/off |",
        "|------|--------:|---------:|-------:|--------:|---------:|---------:|",
    ]
    for lab, fk, sk in (
        ("insert µs/op", "ins", "s_ins"),
        ("update µs/op", "upd", "s_upd"),
        ("get µs/op", "get", "s_get"),
        ("range µs/scan", "rng", "s_rng"),
    ):
        o, b = off[fk], both[fk]
        so, sb = off[sk], both[sk]
        # SQLite 列取两跑均值（同负载抖动）；比值用各自同跑对手
        s_show = (so + sb) / 2 if so == so and sb == sb else (so if so == so else sb)
        lines.append(
            f"| {lab} | {fmt_us(o)} | {fmt_us(b)} | {fmt_us(s_show)} | "
            f"{fmt_r(ratio(o, so))} | {fmt_r(ratio(b, sb))} | {fmt_r(ratio(b, o))} |"
        )
    lines.append(
        f"| space MB | {mb(off['fcs_sp'])} | {mb(both['fcs_sp'])} | {mb(off['sql_sp'])} | "
        f"{fmt_r(ratio(off['fcs_sp'], off['sql_sp']))} | {fmt_r(ratio(both['fcs_sp'], both['sql_sp'] or off['sql_sp']))} | "
        f"{fmt_r(ratio(both['fcs_sp'], off['fcs_sp']))} |"
    )
    lines.append("")

# --- LMDB ---
lines += ["## vs LMDB（bench7）", ""]
for key, title in cfgs:
    off = parse_b7(out / f"bench7_{key}_off.log")
    both = parse_b7(out / f"bench7_{key}_both.log")
    if not off or not both:
        lines += [f"### {title}", "", f"（解析失败：检查 `{key}_*.log`）", ""]
        continue
    lines += [
        f"### {title}",
        "",
        "| 指标 | FCS off | FCS both | LMDB | off/LMDB | both/LMDB | both/off |",
        "|------|--------:|---------:|-----:|---------:|----------:|---------:|",
    ]
    for lab, fk, lk in (
        ("insert µs/op", "ins", "l_ins"),
        ("update µs/op", "upd", "l_upd"),
        ("get µs/op", "get", "l_get"),
        ("range µs/scan", "rng", "l_rng"),
    ):
        o, b = off[fk], both[fk]
        peer = off[lk]
        lines.append(
            f"| {lab} | {fmt_us(o)} | {fmt_us(b)} | {fmt_us(peer)} | "
            f"{fmt_r(ratio(o,peer))} | {fmt_r(ratio(b,peer))} | {fmt_r(ratio(b,o))} |"
        )
    lines.append(
        f"| space MB | {mb(off['fcs_sp'])} | {mb(both['fcs_sp'])} | {mb(off['lmdb_sp'])} | "
        f"{fmt_r(ratio(off['fcs_sp'], off['lmdb_sp']))} | {fmt_r(ratio(both['fcs_sp'], both['lmdb_sp']))} | "
        f"{fmt_r(ratio(both['fcs_sp'], off['fcs_sp']))} |"
    )
    lines.append("")

lines += [
    "## 读法",
    "",
    "- **both/off**：P2 Soft 默认相对显式关的税/收益。",
    "- **FCS/对手**：相对 SQLite/LMDB；&lt;1 写更快。",
    "- Soft sync range vs LMDB 仍是「写换读」大缝；async+cons 整理后收窄。",
    "- **坑 144：P2 已升 Soft 默认**（见 `VERDICT_P2_SOFT_ON.md`）。",
    "",
]
text = "\n".join(lines)
(out / "PEERS_P2.md").write_text(text, encoding="utf-8")
print(text)
PY

echo PEERS_P2_OK out="$OUT"
