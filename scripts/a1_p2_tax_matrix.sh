#!/usr/bin/env bash
# 坑 140：P2 税刀配对消融 + 抗抖动
# - 每轮先 off 再各 cfg（配对 Δ，消系统漂移）
# - 交错多轮；丢弃 warmup；报 median(Δ) + IQR
# - setarch -R 压 ASLR（失败则原样跑）
set -euo pipefail
cd /mnt/e/FCS/embdb/x
mkdir -p backup/soft-a1
REPS="${REPS:-5}"
WARM="${WARM:-1}"
N="${N:-200000}"
OUT=backup/soft-a1
RAW="$OUT/a1_p2_tax_matrix_raw.csv"
SUM="$OUT/a1_p2_tax_matrix_summary.csv"

make build/soft_a1

run_bin() {
  # args after soft_a1: N B VSZ quiet inv p1 bf bf_skip sf sp floor slices maxprom minpend
  if command -v setarch >/dev/null 2>&1; then
    setarch "$(uname -m)" -R ./build/soft_a1 "$@" 2>/dev/null || ./build/soft_a1 "$@"
  else
    ./build/soft_a1 "$@"
  fi
}

parse_log() {
  local log=$1
  local px pt iu uu sfits sprom frags
  px=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_x=/){split($i,a,"=");print a[2]}}')
  pt=$(grep -aE '^== SUMMARY' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^peak_tail=/){split($i,a,"=");print a[2]}}')
  iu=$(grep -aE '^== PHASE_US insert' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^insert_us_op=/){split($i,a,"=");print a[2]}}')
  uu=$(grep -aE '^== PHASE_US update' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^update_us_op=/){split($i,a,"=");print a[2]}}')
  sfits=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_fits=/){split($i,a,"=");print a[2]}}')
  sprom=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^starve_promotes=/){split($i,a,"=");print a[2]}}')
  frags=$(grep -aE '^== maint' "$log" | awk '{for(i=1;i<=NF;i++) if($i~/^part_frags=/){split($i,a,"=");print a[2]}}')
  echo "$px,$pt,$iu,$uu,$sfits,$sprom,$frags"
}

# name:sf:sp:floor:slices:maxprom:minpend
CFGS=(
  "cur:1:1:32:12:1:0"
  "F40:1:1:40:12:1:0"
  "F24:1:1:24:12:1:0"
  "S8:1:1:32:8:1:0"
  "S16:1:1:32:16:1:0"
  "P2:1:1:32:12:2:0"
  "pend64:1:1:32:12:1:64"
  "pend128:1:1:32:12:1:128"
  "F40S8:1:1:40:8:1:0"
)

echo "rep,warm,cfg,peak_x,peak_tail,insert_us,update_us,fits,prom,frags,d_peak_pct,d_insert_pct,d_update_pct" > "$RAW"

TOTAL=$((REPS + WARM))
for ((rep=0; rep<TOTAL; rep++)); do
  warm=0; (( rep < WARM )) && warm=1
  echo "======== rep=$rep warm=$warm ========"
  # off baseline for this rep
  log="$OUT/mtx_r${rep}_off.log"
  run_bin "$N" 500 32 1 0 0 0 0 0 0 0 0 0 0 >"$log" 2>&1
  off=$(parse_log "$log")
  IFS=, read -r opx opt oiu ouu osf osp ofr <<<"$off"
  echo "$rep,$warm,off,$opx,$opt,$oiu,$ouu,$osf,$osp,$ofr,0,0,0" >> "$RAW"
  echo "  off peak_x=$opx insert=$oiu"

  for spec in "${CFGS[@]}"; do
    IFS=: read -r name sf sp flo sl mp mpnd <<<"$spec"
    log="$OUT/mtx_r${rep}_${name}.log"
    run_bin "$N" 500 32 1 0 0 0 0 "$sf" "$sp" "$flo" "$sl" "$mp" "$mpnd" >"$log" 2>&1
    row=$(parse_log "$log")
    IFS=, read -r px pt iu uu sfits sprom frags <<<"$row"
    dp=$(python3 -c "print(f'{(float('$px')/float('$opx')-1)*100:.3f}')")
    di=$(python3 -c "print(f'{(float('$iu')/float('$oiu')-1)*100:.3f}')")
    du=$(python3 -c "print(f'{(float('$uu')/float('$ouu')-1)*100:.3f}')")
    echo "$rep,$warm,$name,$px,$pt,$iu,$uu,$sfits,$sprom,$frags,$dp,$di,$du" >> "$RAW"
    echo "  $name peak_x=$px d_peak=$dp% d_ins=$di% fits=$sfits prom=$sprom"
  done
done

python3 - <<'PY'
import csv, statistics
from pathlib import Path
from collections import defaultdict
rows=[r for r in csv.DictReader(open("backup/soft-a1/a1_p2_tax_matrix_raw.csv"))
      if r["warm"]=="0" and r["cfg"]!="off"]
by=defaultdict(list)
for r in rows:
    by[r["cfg"]].append(r)

def med(xs):
    return statistics.median(xs) if xs else float("nan")
def iqr(xs):
    if len(xs)<2: return 0.0
    q=statistics.quantiles(xs, n=4)
    return q[2]-q[0]

lines=["cfg,n,med_d_peak_pct,iqr_peak,med_d_insert_pct,iqr_ins,med_d_update_pct,med_fits,med_prom,pass_peak,pass_ins,score"]
ver=["# Soft A1 P2 税刀矩阵（坑 140 · 配对 Δ · 去 warmup）","","门槛：med Δpeak ≤ −5% 且 med Δinsert ≤ +10%","",
     "| cfg | med Δpeak | IQR | med Δins | IQR | Δupd | fits | prom | gate |",
     "|-----|----------:|----:|---------:|----:|-----:|-----:|-----:|:----:|"]
scored=[]
for cfg,rs in sorted(by.items()):
    dp=[float(r["d_peak_pct"]) for r in rs]
    di=[float(r["d_insert_pct"]) for r in rs]
    du=[float(r["d_update_pct"]) for r in rs]
    fits=[float(r["fits"]) for r in rs]
    prom=[float(r["prom"]) for r in rs]
    mdp,mdi,mdu=med(dp),med(di),med(du)
    pk_ok = mdp <= -5.0
    in_ok = mdi <= 10.0
    # score: prefer more peak win, less insert tax
    score = (-mdp) - 0.5*max(mdi,0)
    gate = "PASS" if (pk_ok and in_ok) else "fail"
    lines.append(f"{cfg},{len(rs)},{mdp:.3f},{iqr(dp):.3f},{mdi:.3f},{iqr(di):.3f},{mdu:.3f},{med(fits):.1f},{med(prom):.1f},{int(pk_ok)},{int(in_ok)},{score:.3f}")
    ver.append(f"| {cfg} | {mdp:+.2f}% | {iqr(dp):.2f} | {mdi:+.2f}% | {iqr(di):.2f} | {mdu:+.2f}% | {med(fits):.0f} | {med(prom):.0f} | {gate} |")
    scored.append((score, gate, cfg, mdp, mdi))
Path("backup/soft-a1/a1_p2_tax_matrix_summary.csv").write_text("\n".join(lines)+"\n",encoding="utf-8")
scored.sort(reverse=True)
ver += ["","## ranking (score = −Δpeak − 0.5·max(Δins,0)；仅 PASS 可升默认)",""]
for score,gate,cfg,mdp,mdi in scored:
    mark=" ← winner候选" if gate=="PASS" and score==scored[0][0] else ""
    if gate!="PASS": mark=""
    ver.append(f"- `{cfg}` score={score:.2f} peak={mdp:+.2f}% ins={mdi:+.2f}% [{gate}]{mark}")
passers=[x for x in scored if x[1]=="PASS"]
ver += ["", "## leading", ""]
if passers:
    best=passers[0]
    ver.append(f"- **Pareto 胜者（本轮）**：`{best[2]}`  peak med {best[3]:+.2f}% · insert med {best[4]:+.2f}%")
    ver.append("- 若胜者 ≠ `cur`(F32/S12/P1)：把宏默认改到胜者并再跑 ×5 确认。")
else:
    ver.append("- 无 PASS；保持 `cur` 宏，或放宽门槛后重判。")
ver.append("- Soft 默认仍关 P2。")
ver.append("")
Path("backup/soft-a1/VERDICT_P2_TAX_MATRIX.md").write_text("\n".join(ver)+"\n",encoding="utf-8")
print("\n".join(ver))
PY
echo "RAW=$RAW"
echo "SUM=$SUM"
