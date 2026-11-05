#!/usr/bin/env bash
# 完整 Soft Post × tip × dual × sync/async vs SQLite / LMDB（性能+空间）
# 坑 156/161/163/164
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/peers-full}"
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable"
  exit 1
fi

echo "# host: $(uname -a | tr '\n' ' ')" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# Soft Post full ablate: tip×dual×sync/async vs SQL/LMDB" | tee -a "$OUT/META.txt"

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

# argv: … soft=1 sf=1 sp=1 soft_cons=3 dual tip
run_b4() {
  local tag="$1" dual="$2" tip="$3" async="$4" gm="$5" cons="$6" shr="$7" ithr="$8"
  local log="$OUT/b4_${tag}.log"
  echo "== bench4 $tag dual=$dual tip=$tip async=$async =="
  clean_dbs
  ./build/bench4 50000 500 32 50000 2000 100 0 "$async" "$gm" 8 0 "$cons" "$shr" "$ithr" 0 0 1 0 0 0 1 1 3 "$dual" "$tip" \
    2>&1 | tee "$log" | tail -20
}

run_b7() {
  local tag="$1" dual="$2" tip="$3" async="$4" gm="$5" cons="$6" shr="$7"
  local log="$OUT/b7_${tag}.log"
  echo "== bench7 $tag dual=$dual tip=$tip async=$async =="
  clean_dbs
  ./build/bench7 50000 500 32 50000 2000 100 "$async" "$gm" "$cons" "$shr" 1 1 1 3 "$dual" "$tip" \
    2>&1 | tee "$log" | tail -20
}

# Matrix: product tip=1 dual=0; ablate tip; ablate dual (tip=1)
# sync
run_b4 sync_t1d0 0 1 0 0 0 0 1
run_b4 sync_t0d0 0 0 0 0 0 0 1
run_b4 sync_t1d1 1 1 0 0 0 0 1
run_b4 sync_t0d1 1 0 0 0 0 0 1
# async+group+cons+shr
run_b4 async_t1d0 0 1 1 1 1 1 1
run_b4 async_t0d0 0 0 1 1 1 1 1
run_b4 async_t1d1 1 1 1 1 1 1 1
run_b4 async_t0d1 1 0 1 1 1 1 1

run_b7 sync_t1d0 0 1 0 0 0 0
run_b7 sync_t0d0 0 0 0 0 0 0
run_b7 sync_t1d1 1 1 0 0 0 0
run_b7 sync_t0d1 1 0 0 0 0 0
run_b7 async_t1d0 0 1 1 1 1 1
run_b7 async_t0d0 0 0 1 1 1 1
run_b7 async_t1d1 1 1 1 1 1 1
run_b7 async_t0d1 1 0 1 1 1 1

python3 - <<'PY' "$OUT"
import re, sys
from pathlib import Path
out = Path(sys.argv[1])

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
    if not (fm and lm): return {}
    return {
        "ins": float(fm.group(1)), "upd": float(fm.group(2)), "get": float(fm.group(3)), "rng": float(fm.group(4)),
        "l_ins": float(lm.group(1)), "l_upd": float(lm.group(2)), "l_get": float(lm.group(3)), "l_rng": float(lm.group(4)),
        "fcs_sp": int(fm.group(5)), "lmdb_sp": int(lm.group(5)),
    }

def r(a,b):
    return float("nan") if (not b or a!=a or b!=b) else a/b
def fr(x): return "—" if x!=x else f"{x:.2f}×"
def fu(x): return "—" if x!=x else f"{x:.3f}"
def mb(b): return f"{b/1024/1024:.2f}"

arms = [
    ("sync_t1d0", "sync tip=on dual=off (产品)"),
    ("sync_t0d0", "sync tip=off dual=off"),
    ("sync_t1d1", "sync tip=on dual=on"),
    ("sync_t0d1", "sync tip=off dual=on"),
    ("async_t1d0", "async tip=on dual=off (产品)"),
    ("async_t0d0", "async tip=off dual=off"),
    ("async_t1d1", "async tip=on dual=on"),
    ("async_t0d1", "async tip=off dual=on"),
]

lines = [
    "# Soft Post 完整 peers 消融（tip × dual × sync/async）vs SQLite / LMDB",
    "",
    "比值 FCS/对手；&lt;1 = FCS 更快。空间 MB。坑 156/161/163。",
    f"复现：`bash scripts/peers_full_ab.sh` · `{out}/`",
    "",
    "## vs SQLite",
    "",
    "| 臂 | insert | update | get | range | space | /SQL ins | /SQL rng | /SQL sp |",
    "|----|-------:|-------:|----:|------:|------:|---------:|---------:|--------:|",
]
for tag, title in arms:
    p = out / f"b4_{tag}.log"
    d = parse_b4(p)
    lines.append(
        f"| {title} | {fu(d['ins'])} | {fu(d['upd'])} | {fu(d['get'])} | {fu(d['rng'])} | {mb(d['fcs_sp'])} | "
        f"{fr(r(d['ins'], d['s_ins']))} | {fr(r(d['rng'], d['s_rng']))} | {fr(r(d['fcs_sp'], d['sql_sp']))} |"
    )
    # stash SQL baseline from product sync for note
    if tag == "sync_t1d0":
        sql = d

lines += ["", f"SQLite 基线（同 sync_t1d0 跑）：ins={fu(sql['s_ins'])} upd={fu(sql['s_upd'])} get={fu(sql['s_get'])} rng={fu(sql['s_rng'])} sp={mb(sql['sql_sp'])}",
          "", "## vs LMDB", "",
          "| 臂 | insert | update | get | range | space | /LMDB ins | /LMDB get | /LMDB rng | /LMDB sp |",
          "|----|-------:|-------:|----:|------:|------:|----------:|----------:|----------:|---------:|"]
lmdb_base = None
for tag, title in arms:
    d = parse_b7(out / f"b7_{tag}.log")
    if not d:
        lines.append(f"| {title} | parse fail | | | | | | | | |")
        continue
    if tag == "sync_t1d0":
        lmdb_base = d
    lines.append(
        f"| {title} | {fu(d['ins'])} | {fu(d['upd'])} | {fu(d['get'])} | {fu(d['rng'])} | {mb(d['fcs_sp'])} | "
        f"{fr(r(d['ins'], d['l_ins']))} | {fr(r(d['get'], d['l_get']))} | {fr(r(d['rng'], d['l_rng']))} | "
        f"{fr(r(d['fcs_sp'], d['lmdb_sp']))} |"
    )

# tip / dual deltas at sync dual=off / tip=on
t1 = parse_b4(out/"b4_sync_t1d0.log")
t0 = parse_b4(out/"b4_sync_t0d0.log")
d1 = parse_b4(out/"b4_sync_t1d1.log")
lt1 = parse_b7(out/"b7_sync_t1d0.log")
lt0 = parse_b7(out/"b7_sync_t0d0.log")
lines += [
    "",
    "## 消融判决（sync）",
    "",
    f"- tip on/off（dual=off）：insert {fr(r(t1['ins'], t0['ins']))} · update {fr(r(t1['upd'], t0['upd']))} · "
    f"get {fr(r(t1['get'], t0['get']))} · range {fr(r(t1['rng'], t0['rng']))} · space {fr(r(t1['fcs_sp'], t0['fcs_sp']))}",
    f"- dual on/off（tip=on）：insert {fr(r(d1['ins'], t1['ins']))} · update {fr(r(d1['upd'], t1['upd']))} · "
    f"space {fr(r(d1['fcs_sp'], t1['fcs_sp']))}",
    "",
    "## 产品臂（sync tip=on dual=off）",
    "",
]
if lt1:
    lines += [
        f"- vs SQL：ins {fr(r(t1['ins'], t1['s_ins']))} · upd {fr(r(t1['upd'], t1['s_upd']))} · "
        f"get {fr(r(t1['get'], t1['s_get']))} · rng {fr(r(t1['rng'], t1['s_rng']))} · sp {fr(r(t1['fcs_sp'], t1['sql_sp']))}",
        f"- vs LMDB：ins {fr(r(lt1['ins'], lt1['l_ins']))} · get {fr(r(lt1['get'], lt1['l_get']))} · "
        f"rng {fr(r(lt1['rng'], lt1['l_rng']))} · sp {fr(r(lt1['fcs_sp'], lt1['lmdb_sp']))}",
        "",
        "- tip：**KEEP** 默认 on（bloom 跳首写；163 后 g16 绿）" if (t1['ins']<=t0['ins']*1.02) else
        "- tip：若 insert 无胜且中性 → KEEP_NEUTRAL；若回归>5% → Negate",
        "- dual：Soft 默认仍关（Negate-keep）",
        "",
    ]
md = "\n".join(lines)
(out/"PEERS_FULL.md").write_text(md, encoding="utf-8")
print(md)
PY
echo "Wrote $OUT/PEERS_FULL.md"
