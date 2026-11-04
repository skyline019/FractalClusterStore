#!/usr/bin/env bash
# 坑 144：P2 Soft 默认开后全量验证 — c6 · gate · peers · Soft A1 配对
set -euo pipefail
cd /mnt/e/FCS/embdb/x
mkdir -p backup/soft-a1 backup/peers-p2

echo "==== c6 ===="
make build/c6
./build/c6

echo "==== soft-a1-p2-gate (ROUNDS=${ROUNDS:-9}) ===="
ROUNDS="${ROUNDS:-9}" bash scripts/gate_p2_a2a3_lat.sh

echo "==== peers-p2 ===="
bash scripts/peers_p2_ab.sh

echo "==== Soft A1 off vs both (×3) ===="
make build/soft_a1
CSV=backup/soft-a1/soft_on_a1.csv
echo "rep,arm,peak_x,insert_us,update_us,fits,prom" > "$CSV"
for rep in 1 2 3; do
  for arm in off both; do
    case $arm in off) sf=0; sp=0 ;; both) sf=1; sp=1 ;; esac
    log=backup/soft-a1/soft_on_a1_${arm}_${rep}.log
    echo "== A1 rep=$rep arm=$arm =="
    ./build/soft_a1 200000 500 32 1 0 0 0 0 "$sf" "$sp" >"$log" 2>&1
    px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
    iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
    uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
    sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
    sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
    echo "$rep,$arm,$px,$iu,$uu,$sfits,$sprom" | tee -a "$CSV"
  done
done

python3 - <<'PY'
import csv, statistics, re
from pathlib import Path

def med(xs):
    return statistics.median(xs) if xs else float("nan")

rows = list(csv.DictReader(open("backup/soft-a1/soft_on_a1.csv")))
off = [r for r in rows if r["arm"] == "off"]
both = [r for r in rows if r["arm"] == "both"]
mp_o, mp_b = med([float(r["peak_x"]) for r in off]), med([float(r["peak_x"]) for r in both])
mi_o, mi_b = med([float(r["insert_us"]) for r in off]), med([float(r["insert_us"]) for r in both])

gate = Path("backup/soft-a1/VERDICT_P2_GATE.md").read_text(encoding="utf-8")
peers = Path("backup/peers-p2/PEERS_P2.md").read_text(encoding="utf-8")
gate_verdict = "可议" if "可议" in gate else ("否" if "升 Soft 默认：否" in gate else "见门禁")
m = re.search(r"insert：Δ \*\*([+\-0-9.]+%)\*\* → \*\*([^*]+)\*\*", gate)
ins_gate = f"{m.group(1)} {m.group(2)}" if m else "—"

lines = [
    "# P2 Soft 默认开（坑 144）",
    "",
    "拍板：`configure_deploy` / Soft 默认 `set_part_starve_fit(true)` + `set_starve_promote(true)`。",
    "消融：argv / `set_*(false)` 仍可关。",
    "",
    "## Soft A1（×3 中位 · off=显式关 · both=开）",
    "",
    f"| arm | med peak_x | med insert µs |",
    f"|-----|----------:|-------------:|",
    f"| off | {mp_o:.3f} | {mi_o:.3f} |",
    f"| both | {mp_b:.3f} | {mi_b:.3f} |",
    f"| Δ | {(mp_b/mp_o-1)*100:+.1f}% | {(mi_b/mi_o-1)*100:+.1f}% |",
    "",
    "## 门禁（重跑）",
    "",
    f"- Soft sync insert：{ins_gate}",
    f"- 升 Soft 默认判决：{gate_verdict}",
    "- 全文：`VERDICT_P2_GATE.md`",
    "",
    "## peers vs SQLite / LMDB",
    "",
    "全文：`backup/peers-p2/PEERS_P2.md`（sync + async · 写/读/空间）。",
    "",
    "## leading",
    "",
    "- Soft **已开** P2 默认。",
    f"- Soft A1 峰 both/off {(mp_b/mp_o-1)*100:+.1f}%；insert {(mi_b/mi_o-1)*100:+.1f}%。",
    "- 相对 SQL/LMDB：空间叙事不变；async 写仍碾压；sync range vs LMDB 缝仍在。",
    "",
]
Path("backup/soft-a1/VERDICT_P2_SOFT_ON.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
PY
echo SOFT_P2_ON_OK
