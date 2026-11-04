#!/usr/bin/env bash
# 坑 141：β 锁外 fsync 后配对 off vs both（默认税刀含 pend64）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
REPS="${REPS:-5}"
WARM="${WARM:-1}"
N="${N:-200000}"
OUT=backup/soft-a1
RAW="$OUT/a1_p2_lockless_beta_raw.csv"
mkdir -p "$OUT"
make build/soft_a1 build/c6
./build/c6

run_bin() {
  if command -v setarch >/dev/null 2>&1; then
    setarch "$(uname -m)" -R ./build/soft_a1 "$@" 2>/dev/null || ./build/soft_a1 "$@"
  else
    ./build/soft_a1 "$@"
  fi
}
parse() {
  local log=$1
  local px iu uu sfits sprom pms
  px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
  uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
  sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
  sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
  pms=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_prom_ms=/){split($i,a,"=");print a[2]}}')
  echo "$px,$iu,$uu,$sfits,$sprom,${pms:-0}"
}

echo "rep,warm,cfg,peak_x,insert_us,update_us,fits,prom,prom_ms,d_peak_pct,d_insert_pct,d_update_pct" > "$RAW"
TOTAL=$((REPS + WARM))
for ((rep=0; rep<TOTAL; rep++)); do
  warm=0; (( rep < WARM )) && warm=1
  echo "======== lockless-β rep=$rep warm=$warm ========"
  run_bin "$N" 500 32 1 0 0 0 0 0 0 >"$OUT/ll_r${rep}_off.log" 2>&1
  IFS=, read -r opx oiu ouu _ _ _ <<<"$(parse "$OUT/ll_r${rep}_off.log")"
  echo "$rep,$warm,off,$opx,$oiu,$ouu,0,0,0,0,0,0" >> "$RAW"
  echo "  off peak=$opx ins=$oiu"
  # both = 引擎默认（floor32 slices12 prom1 min_pend64 + 锁外 β）
  run_bin "$N" 500 32 1 0 0 0 0 1 1 >"$OUT/ll_r${rep}_both.log" 2>&1
  IFS=, read -r px iu uu sf sp pms <<<"$(parse "$OUT/ll_r${rep}_both.log")"
  dp=$(python3 -c "print(f'{(float('$px')/float('$opx')-1)*100:.3f}')")
  di=$(python3 -c "print(f'{(float('$iu')/float('$oiu')-1)*100:.3f}')")
  du=$(python3 -c "print(f'{(float('$uu')/float('$ouu')-1)*100:.3f}')")
  echo "$rep,$warm,both,$px,$iu,$uu,$sf,$sp,$pms,$dp,$di,$du" >> "$RAW"
  echo "  both peak=$px d_peak=$dp% d_ins=$di% prom=$sp prom_ms=$pms"
done

python3 - <<'PY'
import csv, statistics
from pathlib import Path
rows=[r for r in csv.DictReader(open("backup/soft-a1/a1_p2_lockless_beta_raw.csv")) if r["warm"]=="0" and r["cfg"]=="both"]
dp=[float(r["d_peak_pct"]) for r in rows]
di=[float(r["d_insert_pct"]) for r in rows]
du=[float(r["d_update_pct"]) for r in rows]
pms=[float(r["prom_ms"]) for r in rows]
prom=[float(r["prom"]) for r in rows]
def med(xs): return statistics.median(xs)
def iqr(xs):
  if len(xs)<2: return 0.0
  q=statistics.quantiles(xs,n=4); return q[2]-q[0]
lines=["# Soft A1 β 锁外 fsync（坑 141）","","协议：配对 off→both · warmup 弃 · setarch -R · ×5","",
"| | med | IQR |",
"|--|----:|----:|",
f"| Δpeak | {med(dp):+.2f}% | {iqr(dp):.2f} |",
f"| Δinsert | {med(di):+.2f}% | {iqr(di):.2f} |",
f"| Δupdate | {med(du):+.2f}% | {iqr(du):.2f} |",
f"| prom_ms | {med(pms):.2f} | {iqr(pms):.2f} |",
f"| prom count | {med(prom):.0f} | — |",
"","## leading","",
"- **代码根**：β 旧路径在 `maint_unit` 持 XLock 时 `write_sb`（drain+双 sync）⇒ put 全堵。",
"- **刀**：`starve_prom_flush_` 锁内 fold+capture，锁外 `sb_flush`，再锁内 promote。",
f"- 本轮 med Δpeak {med(dp):+.2f}% · Δins {med(di):+.2f}%（对照坑 140 confirm pend64 peak≈−14%）。",
"- Soft 默认仍关 P2。",""]
Path("backup/soft-a1/VERDICT_P2_LOCKLESS_BETA.md").write_text("\n".join(lines)+"\n",encoding="utf-8")
print("\n".join(lines))
PY
