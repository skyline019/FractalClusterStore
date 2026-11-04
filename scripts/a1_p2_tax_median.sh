#!/usr/bin/env bash
# Soft A1：off vs both ×3 中位（压 Soft 抖动）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
make build/soft_a1
mkdir -p backup/soft-a1
OUT=backup/soft-a1/a1_p2_tax_median.csv
echo "rep,arm,peak_x,peak_tail,insert_us,update_us,fits,prom,frags" > "$OUT"
for rep in 1 2 3; do
  for arm in off both; do
    case $arm in off) sf=0; sp=0 ;; both) sf=1; sp=1 ;; esac
    log=backup/soft-a1/a1_med_${arm}_${rep}.log
    echo "== rep=$rep arm=$arm =="
    ./build/soft_a1 200000 500 32 1 0 0 0 0 "$sf" "$sp" >"$log" 2>&1
    px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
    pt=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_tail=/){split($i,a,"=");print a[2]}}')
    iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
    uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
    sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
    sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
    pf=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^part_frags=/){split($i,a,"=");print a[2]}}')
    echo "$rep,$arm,$px,$pt,$iu,$uu,$sfits,$sprom,$pf" | tee -a "$OUT"
  done
done
python3 - <<'PY'
import csv,statistics
from pathlib import Path
rows=list(csv.DictReader(open("backup/soft-a1/a1_p2_tax_median.csv")))
def med(arm,k):
  xs=[float(r[k]) for r in rows if r["arm"]==arm]
  return statistics.median(xs)
mo,mb=med("off","peak_x"),med("both","peak_x")
io,ib=med("off","insert_us"),med("both","insert_us")
uo,ub=med("off","update_us"),med("both","update_us")
lines=["# Soft A1 写税中位（坑 139 · STARVE_FLOOR=32 MAX_SLICES=12 MAX_PROMOTES=1 · ×3）","",
"| arm | med peak_x | med insert | Δ% | med update | Δ% |",
"|-----|----------:|-----------:|---:|-----------:|---:|",
f"| off | {mo:.3f} | {io:.3f} | 0 | {uo:.3f} | 0 |",
f"| both | {mb:.3f} | {ib:.3f} | {(ib/io-1)*100:+.1f}% | {ub:.3f} | {(ub/uo-1)*100:+.1f}% |",
"", "## raw", "```", Path("backup/soft-a1/a1_p2_tax_median.csv").read_text(), "```", "",
"## leading", "",
f"- peak med {mo:.3f}→{mb:.3f} ({(mb/mo-1)*100:+.1f}%)",
f"- insert med Δ {(ib/io-1)*100:+.1f}%（税刀前单次 both≈+39%）",
f"- update med Δ {(ub/uo-1)*100:+.1f}%",
"- Soft 默认仍关；A2/A3/sync 门禁未在本轮重跑则 KPI_UNVERIFIED。",""]
Path("backup/soft-a1/VERDICT_P2_TAX_A1.md").write_text("\n".join(lines),encoding="utf-8")
print("\n".join(lines))
PY
