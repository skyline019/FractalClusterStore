#!/usr/bin/env bash
# Soft A1 200k：P2 开火画像上量 insert/update us/op + 峰（坑 139 税刀后复测）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
mkdir -p backup/soft-a1
make build/soft_a1
CSV=backup/soft-a1/a1_p2_write_tax.csv
echo "arm,peak_x,peak_tail,insert_us,update_us,starve_fits,starve_promotes,hole_starve,part_frags" > "$CSV"
for arm in off fit prom both; do
  case $arm in
    off) sf=0; sp=0 ;;
    fit) sf=1; sp=0 ;;
    prom) sf=0; sp=1 ;;
    both) sf=1; sp=1 ;;
  esac
  log=backup/soft-a1/a1_tax_${arm}.log
  echo "== Soft A1 arm=$arm =="
  ./build/soft_a1 200000 500 32 1 0 0 0 0 "$sf" "$sp" 2>&1 | tee "$log" | grep -aE '^# Soft|^== SUMMARY|^== PHASE|^== ATTR|^== AT_PEAK|^== maint'
  px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  pt=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_tail=/){split($i,a,"=");print a[2]}}')
  iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
  uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
  sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
  sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
  hs=$(grep -aE '^== ATTR_COUNTS' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^HOLE_STARVE=/){split($i,a,"=");print a[2]}}')
  pf=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^part_frags=/){split($i,a,"=");print a[2]}}')
  echo "$arm,$px,$pt,$iu,$uu,$sfits,$sprom,$hs,$pf" >> "$CSV"
done
echo "--- CSV ---"
cat "$CSV"
python3 - <<'PY'
import csv
from pathlib import Path
rows=list(csv.DictReader(open("backup/soft-a1/a1_p2_write_tax.csv")))
off={r["arm"]:r for r in rows}["off"]
def f(x):
  try: return float(x)
  except: return float("nan")
lines=["# Soft A1 写税（坑 139 税刀后 · N=200k）","","| arm | peak_x | insert µs/op | Δ% | update µs/op | Δ% | fits | prom | frags | STARVE |",
"|-----|-------:|-------------:|---:|-------------:|---:|-----:|-----:|------:|-------:|"]
for r in rows:
  oi,ou=f(off["insert_us"]),f(off["update_us"])
  ii,uu=f(r["insert_us"]),f(r["update_us"])
  di=(ii/oi-1)*100 if oi else float("nan")
  du=(uu/ou-1)*100 if ou else float("nan")
  lines.append(f"| {r['arm']} | {r['peak_x']} | {r['insert_us']} | {di:+.1f}% | {r['update_us']} | {du:+.1f}% | {r['starve_fits']} | {r['starve_promotes']} | {r['part_frags']} | {r['hole_starve']} |")
fit=next(r for r in rows if r["arm"]=="fit")
prom=next(r for r in rows if r["arm"]=="prom")
both=next(r for r in rows if r["arm"]=="both")
di_f=(f(fit["insert_us"])/f(off["insert_us"])-1)*100
di_b=(f(both["insert_us"])/f(off["insert_us"])-1)*100
px_b=f(both["peak_x"]); px_o=f(off["peak_x"])
lines += ["","## leading","",
f"- pre-cut 对照：fit insert≈+35% / both≈+39% / prom 计数 13（见先验 a1 日志）",
f"- 本轮 both peak_x {px_o:.3f}→{px_b:.3f} ({(px_b/px_o-1)*100:+.1f}%)；insert Δ both={di_b:+.1f}% fit={di_f:+.1f}%",
f"- both fits={both['starve_fits']} prom={both['starve_promotes']} frags={both['part_frags']}",""]
if di_b < 20 and px_b < px_o * 0.95:
  lines.append("- **税刀有效**：insert 税压到 <+20% 且峰仍降。")
elif di_b < di_f and f(both["starve_promotes"]) <= 4:
  lines.append("- β 封顶生效（prom≤4）；insert 税仍看 α。")
else:
  lines.append("- 继续盯：若峰回吐或税仍>25%，调 STARVE_FLOOR/MAX_SLICES 或撤 β 预算。")
lines.append("")
Path("backup/soft-a1/VERDICT_P2_TAX_A1.md").write_text("\n".join(lines)+"\n",encoding="utf-8")
print("\n".join(lines))
PY
