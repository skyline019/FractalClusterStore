#!/usr/bin/env bash
# 坑 151：Soft 默认升 Post 门禁 — 消融 · Soft A1 · peers
# 判据（DISCUSS_RANGE_ROOT §4.2 对齐）：
#   1) ablate：200k Post insert/Soft ≤1.15；cursors≤2；peak_x 写期见 A1
#   2) Soft A1 ×3：med peak_x Post/Off ≤1.05；med insert ≤1.15
#   3) peers：已有 PEERS_POST（sync+repay range 胜、space 不劣 LMDB）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p backup/soft-a1 backup/peers-post backup/range-opt/root

echo "==== ablate_range_idle (soft vs post) ===="
bash scripts/ablate_range_idle.sh

echo "==== Soft A1 Off vs Post (×3) ===="
make build/soft_a1
CSV=backup/soft-a1/soft_post_a1.csv
echo "rep,arm,peak_x,insert_us,update_us" > "$CSV"
for rep in 1 2 3; do
  for arm in off post; do
    case $arm in off) sc=0 ;; post) sc=3 ;; esac
    log=backup/soft-a1/soft_post_a1_${arm}_${rep}.log
    echo "== A1 rep=$rep arm=$arm soft_cons=$sc =="
    # argv: N B VSZ quiet inv p1 bf bf_skip sf sp floor slices maxprom minpend soft_cons
    ./build/soft_a1 200000 500 32 1 0 0 0 0 -1 -1 0 0 0 0 "$sc" >"$log" 2>&1
    px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
    iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
    uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
    echo "$rep,$arm,$px,$iu,$uu" | tee -a "$CSV"
  done
done

echo "==== peers_post ===="
bash scripts/peers_post_ab.sh

python3 - <<'PY'
import csv, statistics, re
from pathlib import Path

def med(xs):
    return statistics.median(xs) if xs else float("nan")

rows = list(csv.DictReader(open("backup/soft-a1/soft_post_a1.csv")))
off = [r for r in rows if r["arm"] == "off"]
post = [r for r in rows if r["arm"] == "post"]
mp_o, mp_p = med([float(r["peak_x"]) for r in off]), med([float(r["peak_x"]) for r in post])
mi_o, mi_p = med([float(r["insert_us"]) for r in off]), med([float(r["insert_us"]) for r in post])
peak_r = mp_p / mp_o if mp_o else float("nan")
ins_r = mi_p / mi_o if mi_o else float("nan")

# ablate csv
abl = Path("backup/range-opt/root/idle_write_tax.csv").read_text(encoding="utf-8").strip().splitlines()
hdr = abl[0].split(",")
idx = {h: i for i, h in enumerate(hdr)}
soft200 = post200 = None
for line in abl[1:]:
    c = line.split(",")
    if c[idx["arm"]] == "soft" and c[idx["N"]] == "200000":
        soft200 = c
    if c[idx["arm"]] == "post" and c[idx["N"]] == "200000" and c[idx["gap"]] == "32":
        post200 = c
abl_ins_r = float(post200[idx["insert"]]) / float(soft200[idx["insert"]]) if soft200 and post200 else float("nan")
abl_cur = float(post200[idx["cursors"]]) if post200 else float("nan")

peers = Path("backup/peers-post/PEERS_POST.md").read_text(encoding="utf-8")
# soft sync Post/SQL range row
m = re.search(r"Soft sync 50k.*?range µs/scan \|[^\n]*\| ([0-9.]+)× \| ([0-9.]+)×", peers, re.S)
sql_base_r = float(m.group(1)) if m else float("nan")
sql_post_r = float(m.group(2)) if m else float("nan")

gate_abl_ins = abl_ins_r <= 1.15
gate_abl_cur = abl_cur <= 2.0
gate_a1_peak = peak_r <= 1.05
gate_a1_ins = ins_r <= 1.15
gate_peers = sql_post_r < sql_base_r and sql_post_r == sql_post_r
ok = all([gate_abl_ins, gate_abl_cur, gate_a1_peak, gate_a1_ins, gate_peers])

def yn(b):
    return "PASS" if b else "FAIL"

lines = [
    "# Soft 默认升 Post 门禁（坑 151）",
    "",
    "判据：消融 insert≤+15% · cursors≤2 · Soft A1 peak≤+5% / insert≤+15% · peers sync range 优于 Soft 基线。",
    "",
    "## 1. 消融（`ablate_range_idle` · 200k gap32）",
    "",
    f"| | Soft | Post | Post/Soft |",
    f"|--|------:|-----:|----------:|",
    f"| insert | {soft200[idx['insert']] if soft200 else '—'} | {post200[idx['insert']] if post200 else '—'} | {abl_ins_r:.3f}× |",
    f"| cursors | {soft200[idx['cursors']] if soft200 else '—'} | {abl_cur:.2f} | — |",
    f"| 门禁 | | | {yn(gate_abl_ins and gate_abl_cur)} |",
    "",
    "## 2. Soft A1（×3 中位 · 写期无 repay）",
    "",
    f"| arm | med peak_x | med insert µs |",
    f"|-----|----------:|-------------:|",
    f"| off | {mp_o:.3f} | {mi_o:.3f} |",
    f"| post | {mp_p:.3f} | {mi_p:.3f} |",
    f"| Δ | {(peak_r-1)*100:+.1f}% | {(ins_r-1)*100:+.1f}% |",
    f"| 门禁 | {yn(gate_a1_peak)} | {yn(gate_a1_ins)} |",
    "",
    "## 3. peers Soft sync（`PEERS_POST.md`）",
    "",
    f"- range FCS/SQL：base **{sql_base_r:.2f}×** → Post **{sql_post_r:.2f}×** · {yn(gate_peers)}",
    "- 全文：`backup/peers-post/PEERS_POST.md`",
    "",
    f"## 判决：**{'升 Soft 默认：可议通过' if ok else '升 Soft 默认：否'}**",
    "",
    "通过后：`configure_deploy` / Soft → `SoftCons::Post`；读前 `soft_cons_repay` 或 `polish`；消融 `set_soft_cons(Off)`。",
    "",
]
Path("backup/soft-a1/VERDICT_POST_SOFT_ON.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
raise SystemExit(0 if ok else 1)
PY
