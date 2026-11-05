#!/usr/bin/env bash
# 完整 Soft Post peers：tip×dual×sync/async × R 轮中位 vs SQLite/LMDB
# 输出 backup/peers-full/PEERS_FULL_MED.md
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/peers-full}"
R="${R:-5}"
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable"
  exit 1
fi

echo "# peers_full_median R=$R host=$(uname -n) date=$(date -Iseconds)" | tee "$OUT/META_MED.txt"

g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench7.cpp -o build/bench7 -llmdb

clean_dbs() {
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm \
        /home/user/.fcs_peers.db
  rm -rf /home/user/.lmdb_peers
}

# tag dual tip async gm cons shr ithr
run_b4() {
  local tag="$1" dual="$2" tip="$3" async="$4" gm="$5" cons="$6" shr="$7" ithr="$8" round="$9"
  local log="$OUT/med_b4_${tag}_r${round}.log"
  clean_dbs
  ./build/bench4 50000 500 32 50000 2000 100 0 "$async" "$gm" 8 0 "$cons" "$shr" "$ithr" 0 0 1 0 0 0 1 1 3 "$dual" "$tip" \
    >"$log" 2>&1
}

run_b7() {
  local tag="$1" dual="$2" tip="$3" async="$4" gm="$5" cons="$6" shr="$7" round="$8"
  local log="$OUT/med_b7_${tag}_r${round}.log"
  clean_dbs
  ./build/bench7 50000 500 32 50000 2000 100 "$async" "$gm" "$cons" "$shr" 1 1 1 3 "$dual" "$tip" \
    >"$log" 2>&1
}

ARMS=(
  "sync_t1d0 0 1 0 0 0 0 1"
  "sync_t0d0 0 0 0 0 0 0 1"
  "sync_t1d1 1 1 0 0 0 0 1"
  "async_t1d0 0 1 1 1 1 1 1"
)

for ((r=1; r<=R; r++)); do
  echo "== round $r/$R =="
  for arm in "${ARMS[@]}"; do
    set -- $arm
    tag=$1 dual=$2 tip=$3 async=$4 gm=$5 cons=$6 shr=$7 ithr=$8
    echo "  b4 $tag"
    run_b4 "$tag" "$dual" "$tip" "$async" "$gm" "$cons" "$shr" "$ithr" "$r"
    echo "  b7 $tag"
    run_b7 "$tag" "$dual" "$tip" "$async" "$gm" "$cons" "$shr" "$r"
  done
done

python3 - <<'PY' "$OUT" "$R"
import re, sys
from pathlib import Path
out = Path(sys.argv[1]); R = int(sys.argv[2])

def med(xs):
    xs = sorted(xs)
    return xs[len(xs)//2]

def parse_b4(path):
    t = path.read_text(errors="replace")
    def us(phase, who="fcs"):
        m = re.search(rf"^{who} {phase}\s+([0-9.]+)", t, re.M)
        return float(m.group(1)) if m else float("nan")
    m = re.search(r"^space bytes\s+(\d+)\s+(\d+)", t, re.M)
    return {
        "ins": us("insert"), "upd": us("update"), "get": us("get"), "rng": us("range"),
        "s_ins": us("insert","sqlite"), "s_upd": us("update","sqlite"),
        "s_get": us("get","sqlite"), "s_rng": us("range","sqlite"),
        "fcs_sp": int(m.group(1)) if m else 0, "sql_sp": int(m.group(2)) if m else 0,
    }

def parse_b7(path):
    t = path.read_text(errors="replace")
    fm = re.search(r"# fcs done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    lm = re.search(r"# lmdb done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    if not (fm and lm): return None
    return {
        "ins": float(fm.group(1)), "upd": float(fm.group(2)), "get": float(fm.group(3)), "rng": float(fm.group(4)),
        "l_ins": float(lm.group(1)), "l_upd": float(lm.group(2)), "l_get": float(lm.group(3)), "l_rng": float(lm.group(4)),
        "fcs_sp": int(fm.group(5)), "lmdb_sp": int(lm.group(5)),
    }

def gather_b4(tag):
    rows = [parse_b4(out / f"med_b4_{tag}_r{i}.log") for i in range(1, R+1)]
    keys = ["ins","upd","get","rng","s_ins","s_upd","s_get","s_rng","fcs_sp","sql_sp"]
    return {k: med([r[k] for r in rows]) for k in keys}

def gather_b7(tag):
    rows = [parse_b7(out / f"med_b7_{tag}_r{i}.log") for i in range(1, R+1)]
    rows = [r for r in rows if r]
    keys = ["ins","upd","get","rng","l_ins","l_upd","l_get","l_rng","fcs_sp","lmdb_sp"]
    return {k: med([r[k] for r in rows]) for k in keys}

def r(a,b):
    return float("nan") if (not b or a!=a or b!=b) else a/b
def fr(x): return "—" if x!=x else f"{x:.2f}×"
def fu(x): return "—" if x!=x else f"{x:.3f}"
def mb(b): return f"{b/1024/1024:.2f}"

arms = [
    ("sync_t1d0", "sync tip=on dual=off (产品)"),
    ("sync_t0d0", "sync tip=off dual=off"),
    ("sync_t1d1", "sync tip=on dual=on"),
    ("async_t1d0", "async tip=on dual=off (产品)"),
]

lines = [
    f"# Soft Post 完整 peers 消融（中位 ×{R}）vs SQLite / LMDB",
    "",
    "比值 FCS/对手；<1 = FCS 更快。空间 MB。坑 163/165/166。",
    f"复现：`R={R} bash scripts/peers_full_median.sh` · `{out}/`",
    "",
    "## vs SQLite（中位）",
    "",
    "| 臂 | insert | update | get | range | space | /SQL ins | /SQL upd | /SQL get | /SQL rng | /SQL sp |",
    "|----|-------:|-------:|----:|------:|------:|---------:|---------:|---------:|---------:|--------:|",
]
b4 = {}
for tag, title in arms:
    d = gather_b4(tag); b4[tag] = d
    lines.append(
        f"| {title} | {fu(d['ins'])} | {fu(d['upd'])} | {fu(d['get'])} | {fu(d['rng'])} | {mb(d['fcs_sp'])} | "
        f"{fr(r(d['ins'], d['s_ins']))} | {fr(r(d['upd'], d['s_upd']))} | {fr(r(d['get'], d['s_get']))} | "
        f"{fr(r(d['rng'], d['s_rng']))} | {fr(r(d['fcs_sp'], d['sql_sp']))} |"
    )

lines += ["", "## vs LMDB（中位）", "",
          "| 臂 | insert | update | get | range | space | /LMDB ins | /LMDB upd | /LMDB get | /LMDB rng | /LMDB sp |",
          "|----|-------:|-------:|----:|------:|------:|----------:|----------:|----------:|----------:|---------:|"]
b7 = {}
for tag, title in arms:
    d = gather_b7(tag); b7[tag] = d
    lines.append(
        f"| {title} | {fu(d['ins'])} | {fu(d['upd'])} | {fu(d['get'])} | {fu(d['rng'])} | {mb(d['fcs_sp'])} | "
        f"{fr(r(d['ins'], d['l_ins']))} | {fr(r(d['upd'], d['l_upd']))} | {fr(r(d['get'], d['l_get']))} | "
        f"{fr(r(d['rng'], d['l_rng']))} | {fr(r(d['fcs_sp'], d['lmdb_sp']))} |"
    )

t1, t0, d1 = b4["sync_t1d0"], b4["sync_t0d0"], b4["sync_t1d1"]
lt1 = b7["sync_t1d0"]
at1, alt1 = b4["async_t1d0"], b7["async_t1d0"]
lines += [
    "",
    "## 消融判决（sync 中位）",
    "",
    f"- tip on/off（dual=off）：insert {fr(r(t1['ins'], t0['ins']))} · update {fr(r(t1['upd'], t0['upd']))} · "
    f"get {fr(r(t1['get'], t0['get']))} · range {fr(r(t1['rng'], t0['rng']))} · space {fr(r(t1['fcs_sp'], t0['fcs_sp']))}",
    f"- dual on/off（tip=on）：insert {fr(r(d1['ins'], t1['ins']))} · update {fr(r(d1['upd'], t1['upd']))} · "
    f"get {fr(r(d1['get'], t1['get']))} · range {fr(r(d1['rng'], t1['rng']))} · space {fr(r(d1['fcs_sp'], t1['fcs_sp']))}",
    "",
    "## 产品臂（中位）",
    "",
    "### sync tip=on dual=off",
    f"- vs SQL：ins {fr(r(t1['ins'], t1['s_ins']))} · upd {fr(r(t1['upd'], t1['s_upd']))} · "
    f"get {fr(r(t1['get'], t1['s_get']))} · rng {fr(r(t1['rng'], t1['s_rng']))} · sp {fr(r(t1['fcs_sp'], t1['sql_sp']))}",
    f"- vs LMDB：ins {fr(r(lt1['ins'], lt1['l_ins']))} · upd {fr(r(lt1['upd'], lt1['l_upd']))} · "
    f"get {fr(r(lt1['get'], lt1['l_get']))} · rng {fr(r(lt1['rng'], lt1['l_rng']))} · sp {fr(r(lt1['fcs_sp'], lt1['lmdb_sp']))}",
    "",
    "### async tip=on dual=off",
    f"- vs SQL：ins {fr(r(at1['ins'], at1['s_ins']))} · upd {fr(r(at1['upd'], at1['s_upd']))} · "
    f"get {fr(r(at1['get'], at1['s_get']))} · rng {fr(r(at1['rng'], at1['s_rng']))} · sp {fr(r(at1['fcs_sp'], at1['sql_sp']))}",
    f"- vs LMDB：ins {fr(r(alt1['ins'], alt1['l_ins']))} · upd {fr(r(alt1['upd'], alt1['l_upd']))} · "
    f"get {fr(r(alt1['get'], alt1['l_get']))} · rng {fr(r(alt1['rng'], alt1['l_rng']))} · sp {fr(r(alt1['fcs_sp'], alt1['lmdb_sp']))}",
    "",
    "- tip：**KEEP**（163 bloom；中位 insert 相对 tip-off 无稳定回归才维持；见上 tip 行）",
    "- dual：Soft 默认仍关（Negate-keep）",
    "",
]
md = "\n".join(lines)
(out/"PEERS_FULL_MED.md").write_text(md, encoding="utf-8")
(out/"PEERS_FULL.md").write_text(md, encoding="utf-8")
print(md)
PY
echo "Wrote $OUT/PEERS_FULL_MED.md"
