#!/usr/bin/env bash
# 坑 156：Soft Post × dual_mem on/off vs SQLite / LMDB
# dual 仅 Soft sync 生效（async 路径不走 flush_mem_dual）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/peers-dual}"
mkdir -p "$OUT" build

if [[ ! -w /home/user ]]; then
  echo "ERROR: /home/user not writable"
  exit 1
fi

echo "# host: $(uname -a | tr '\n' ' ')" | tee "$OUT/META.txt"
echo "# date: $(date -Iseconds)" | tee -a "$OUT/META.txt"
echo "# Soft Post + dual_mem ablate vs SQL/LMDB (pit 156 S8)" | tee -a "$OUT/META.txt"

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

# soft_cons=3 Post · P2 on · dual_mem=$dual
# bench4 argv … soft=1 sp=0 bf=0 skip=0 sf=1 sp=1 soft_cons dual
run_b4() {
  local tag="$1" dual="$2" async="$3" gm="$4" cons="$5" shr="$6" ithr="$7"
  local log="$OUT/bench4_${tag}.log"
  echo "== bench4 $tag dual=$dual =="
  clean_dbs
  ./build/bench4 50000 500 32 50000 2000 100 0 "$async" "$gm" 8 0 "$cons" "$shr" "$ithr" 0 0 1 0 0 0 1 1 3 "$dual" \
    2>&1 | tee "$log" | tail -30
}

run_b7() {
  local tag="$1" dual="$2" async="$3" gm="$4" cons="$5" shr="$6"
  local log="$OUT/bench7_${tag}.log"
  echo "== bench7 $tag dual=$dual =="
  clean_dbs
  ./build/bench7 50000 500 32 50000 2000 100 "$async" "$gm" "$cons" "$shr" 1 1 1 3 "$dual" \
    2>&1 | tee "$log" | tail -30
}

run_b4 soft_sync_off 0 0 0 0 0 1
run_b4 soft_sync_on  1 0 0 0 0 1
run_b4 soft_async_off 0 1 1 1 1 1
run_b4 soft_async_on  1 1 1 1 1 1

run_b7 soft_sync_off 0 0 0 0 0
run_b7 soft_sync_on  1 0 0 0 0
run_b7 soft_async_off 0 1 1 1 1
run_b7 soft_async_on  1 1 1 1 1

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
    dm = re.search(r"# fcs dual_mem: flushes=(\d+) excl_us=(\d+) enc_us=(\d+) settle_us=(\d+)", t)
    return {
        "ins": us("insert"), "upd": us("update"), "get": us("get"), "rng": us("range"),
        "s_ins": sq("insert"), "s_upd": sq("update"), "s_get": sq("get"), "s_rng": sq("range"),
        "fcs_sp": fcs_sp, "sql_sp": sql_sp,
        "flushes": int(dm.group(1)) if dm else 0,
        "excl": int(dm.group(2)) if dm else 0,
        "settle": int(dm.group(4)) if dm else 0,
    }

def parse_b7(path):
    t = path.read_text(errors="replace")
    fm = re.search(r"# fcs done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    lm = re.search(r"# lmdb done insert=([0-9.]+) update=([0-9.]+) get=([0-9.]+) range=([0-9.]+) space=(\d+)", t)
    dm = re.search(r"# fcs dual_mem: flushes=(\d+) excl_us=(\d+) enc_us=(\d+) settle_us=(\d+)", t)
    if not (fm and lm):
        return {}
    return {
        "ins": float(fm.group(1)), "upd": float(fm.group(2)), "get": float(fm.group(3)), "rng": float(fm.group(4)),
        "l_ins": float(lm.group(1)), "l_upd": float(lm.group(2)), "l_get": float(lm.group(3)), "l_rng": float(lm.group(4)),
        "fcs_sp": int(fm.group(5)), "lmdb_sp": int(lm.group(5)),
        "flushes": int(dm.group(1)) if dm else 0,
        "excl": int(dm.group(2)) if dm else 0,
        "settle": int(dm.group(4)) if dm else 0,
    }

def ratio(a, b):
    if not b or a != a or b != b: return float("nan")
    return a / b

def fmt_r(x):
    return "—" if x != x else f"{x:.2f}×"

def fmt_us(x):
    return "—" if x != x else f"{x:.3f}"

def mb(b):
    return f"{b/1024/1024:.2f}"

arms = [("off", "dual=off"), ("on", "dual=on")]
cfgs = [
    ("soft_sync", "Soft sync Post 50k"),
    ("soft_async", "Soft async+group+cons+shr Post 50k"),
]

lines = [
    "# Soft Post × dual_mem vs SQLite / LMDB（坑 156）",
    "",
    "产品 Soft = Post + `soft_cons_repay`；`dual_mem` 可选（仅 sync 走 `flush_mem_dual`）。",
    "比值 = FCS/对手；&lt;1 = FCS 更快。空间 MB。",
    f"复现：`bash scripts/peers_dual_ab.sh` · 源 `{out}/`",
    "",
]

for cfg, title in cfgs:
    lines += [f"## vs SQLite · {title}", "",
              "| 指标 | dual=off | dual=on | SQLite | off/SQL | on/SQL | on/off |",
              "|------|--------:|--------:|-------:|--------:|-------:|-------:|"]
    off = parse_b4(out / f"bench4_{cfg}_off.log")
    on = parse_b4(out / f"bench4_{cfg}_on.log")
    # on/SQL uses off-run peer (same SQL baseline; peer noise otherwise skews on/SQL)
    for key, label in [("ins","insert µs/op"),("upd","update µs/op"),("get","get µs/op"),("rng","range µs/scan")]:
        sk = {"ins":"s_ins","upd":"s_upd","get":"s_get","rng":"s_rng"}[key]
        lines.append(
            f"| {label} | {fmt_us(off[key])} | {fmt_us(on[key])} | {fmt_us(off[sk])} | "
            f"{fmt_r(ratio(off[key], off[sk]))} | {fmt_r(ratio(on[key], off[sk]))} | {fmt_r(ratio(on[key], off[key]))} |"
        )
    lines.append(
        f"| space MB | {mb(off['fcs_sp'])} | {mb(on['fcs_sp'])} | {mb(off['sql_sp'])} | "
        f"{fmt_r(ratio(off['fcs_sp'], off['sql_sp']))} | {fmt_r(ratio(on['fcs_sp'], on['sql_sp']))} | "
        f"{fmt_r(ratio(on['fcs_sp'], off['fcs_sp']))} |"
    )
    if on.get("flushes"):
        lines.append(f"| dual flushes / excl_us / settle_us | — | {on['flushes']} / {on['excl']} / {on['settle']} | — | — | — | — |")
    lines.append("")

for cfg, title in cfgs:
    lines += [f"## vs LMDB · {title}", "",
              "| 指标 | dual=off | dual=on | LMDB | off/LMDB | on/LMDB | on/off |",
              "|------|--------:|--------:|-----:|---------:|--------:|-------:|"]
    off = parse_b7(out / f"bench7_{cfg}_off.log")
    on = parse_b7(out / f"bench7_{cfg}_on.log")
    if not off or not on:
        lines += ["| (parse fail) | | | | | | |", ""]
        continue
    for key, label in [("ins","insert µs/op"),("upd","update µs/op"),("get","get µs/op"),("rng","range µs/scan")]:
        lk = {"ins":"l_ins","upd":"l_upd","get":"l_get","rng":"l_rng"}[key]
        lines.append(
            f"| {label} | {fmt_us(off[key])} | {fmt_us(on[key])} | {fmt_us(off[lk])} | "
            f"{fmt_r(ratio(off[key], off[lk]))} | {fmt_r(ratio(on[key], off[lk]))} | {fmt_r(ratio(on[key], off[key]))} |"
        )
    lines.append(
        f"| space MB | {mb(off['fcs_sp'])} | {mb(on['fcs_sp'])} | {mb(off['lmdb_sp'])} | "
        f"{fmt_r(ratio(off['fcs_sp'], off['lmdb_sp']))} | {fmt_r(ratio(on['fcs_sp'], on['lmdb_sp']))} | "
        f"{fmt_r(ratio(on['fcs_sp'], off['fcs_sp']))} |"
    )
    if on.get("flushes"):
        lines.append(f"| dual flushes / excl_us / settle_us | — | {on['flushes']} / {on['excl']} / {on['settle']} | — | — | — | — |")
    lines.append("")

lines += [
    "## 读法",
    "",
    "- **on/off**：dual 相对产品 Soft Post；≈1 = 中性；sync 才应见 dual flush 计数。",
    "- **async+dual**：引擎不走 dual 路径（`try_dual` 要求 `!async_`）⇒ on≈off 属预期。",
    "- Soft 默认仍 **关** dual（Negate-keep）。",
    "",
]
md = "\n".join(lines)
(out / "PEERS_DUAL.md").write_text(md, encoding="utf-8")
print(md)
PY
echo "Wrote $OUT/PEERS_DUAL.md"
