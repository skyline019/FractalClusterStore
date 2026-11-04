#!/usr/bin/env bash
# 坑 140：确认胜者 pend64 vs cur vs off（配对 ×REPS，去 warmup）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
REPS="${REPS:-5}"
WARM="${WARM:-1}"
N="${N:-200000}"
OUT=backup/soft-a1
RAW="$OUT/a1_p2_tax_confirm_raw.csv"
mkdir -p "$OUT"
make build/soft_a1

run_bin() {
  if command -v setarch >/dev/null 2>&1; then
    setarch "$(uname -m)" -R ./build/soft_a1 "$@" 2>/dev/null || ./build/soft_a1 "$@"
  else
    ./build/soft_a1 "$@"
  fi
}
parse() {
  local log=$1
  local px iu uu sfits sprom
  px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
  uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
  sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
  sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
  echo "$px,$iu,$uu,$sfits,$sprom"
}

echo "rep,warm,cfg,peak_x,insert_us,update_us,fits,prom,d_peak_pct,d_insert_pct,d_update_pct" > "$RAW"
TOTAL=$((REPS + WARM))
for ((rep=0; rep<TOTAL; rep++)); do
  warm=0; (( rep < WARM )) && warm=1
  echo "======== confirm rep=$rep warm=$warm ========"
  # off
  run_bin "$N" 500 32 1 0 0 0 0 0 0 >"$OUT/cfm_r${rep}_off.log" 2>&1
  IFS=, read -r opx oiu ouu _ _ <<<"$(parse "$OUT/cfm_r${rep}_off.log")"
  echo "$rep,$warm,off,$opx,$oiu,$ouu,0,0,0,0,0" >> "$RAW"
  echo "  off peak=$opx ins=$oiu"
  # cur = F32/S12/P1 min_pend=0（显式关门槛）
  run_bin "$N" 500 32 1 0 0 0 0 1 1 32 12 1 0 >"$OUT/cfm_r${rep}_cur.log" 2>&1
  IFS=, read -r px iu uu sf sp <<<"$(parse "$OUT/cfm_r${rep}_cur.log")"
  dp=$(python3 -c "print(f'{(float('$px')/float('$opx')-1)*100:.3f}')")
  di=$(python3 -c "print(f'{(float('$iu')/float('$oiu')-1)*100:.3f}')")
  du=$(python3 -c "print(f'{(float('$uu')/float('$ouu')-1)*100:.3f}')")
  echo "$rep,$warm,cur,$px,$iu,$uu,$sf,$sp,$dp,$di,$du" >> "$RAW"
  echo "  cur  peak=$px d_peak=$dp% d_ins=$di%"
  # pend64 = 引擎新默认（argv 不传 min_pend → 宏 64）；显式传 64 更稳
  run_bin "$N" 500 32 1 0 0 0 0 1 1 32 12 1 64 >"$OUT/cfm_r${rep}_pend64.log" 2>&1
  IFS=, read -r px iu uu sf sp <<<"$(parse "$OUT/cfm_r${rep}_pend64.log")"
  dp=$(python3 -c "print(f'{(float('$px')/float('$opx')-1)*100:.3f}')")
  di=$(python3 -c "print(f'{(float('$iu')/float('$oiu')-1)*100:.3f}')")
  du=$(python3 -c "print(f'{(float('$uu')/float('$ouu')-1)*100:.3f}')")
  echo "$rep,$warm,pend64,$px,$iu,$uu,$sf,$sp,$dp,$di,$du" >> "$RAW"
  echo "  p64  peak=$px d_peak=$dp% d_ins=$di% fits=$sf prom=$sp"
done

python3 - <<'PY'
import csv, statistics
from pathlib import Path
from collections import defaultdict
rows=[r for r in csv.DictReader(open("backup/soft-a1/a1_p2_tax_confirm_raw.csv")) if r["warm"]=="0" and r["cfg"]!="off"]
by=defaultdict(list)
for r in rows: by[r["cfg"]].append(r)
def med(xs): return statistics.median(xs)
def iqr(xs):
  if len(xs)<2: return 0.0
  q=statistics.quantiles(xs,n=4); return q[2]-q[0]
lines=["# Soft A1 P2 税刀确认（坑 140 · pend64 升默认后 ×5 配对）","",
"| cfg | med Δpeak | IQR | med Δins | IQR | Δupd | fits | prom |",
"|-----|----------:|----:|---------:|----:|-----:|-----:|-----:|"]
for cfg in ("cur","pend64"):
  rs=by[cfg]
  dp=[float(r["d_peak_pct"]) for r in rs]; di=[float(r["d_insert_pct"]) for r in rs]
  du=[float(r["d_update_pct"]) for r in rs]
  fits=[float(r["fits"]) for r in rs]; prom=[float(r["prom"]) for r in rs]
  lines.append(f"| {cfg} | {med(dp):+.2f}% | {iqr(dp):.2f} | {med(di):+.2f}% | {iqr(di):.2f} | {med(du):+.2f}% | {med(fits):.0f} | {med(prom):.0f} |")
c,p=by["cur"],by["pend64"]
mdp_c,mdp_p=med([float(r["d_peak_pct"]) for r in c]),med([float(r["d_peak_pct"]) for r in p])
mdi_c,mdi_p=med([float(r["d_insert_pct"]) for r in c]),med([float(r["d_insert_pct"]) for r in p])
lines += ["","## leading","",
f"- 矩阵胜者 `pend64` 确认：peak med {mdp_p:+.2f}% · insert {mdi_p:+.2f}%（对照 cur {mdp_c:+.2f}% / {mdi_c:+.2f}%）",
"- 默认：`FCS_PART_STARVE_PROM_MIN_PEND=64`；FLOOR=32 SLICES=12 PROMOTES=1。",
"- Soft 默认仍关 P2（sync update 门禁未重开）。",""]
Path("backup/soft-a1/VERDICT_P2_TAX_CONFIRM.md").write_text("\n".join(lines)+"\n",encoding="utf-8")
print("\n".join(lines))
PY
