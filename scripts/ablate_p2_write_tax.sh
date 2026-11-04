#!/usr/bin/env bash
# 坑 138：写税归因消融 — Soft sync 交错 off / fit / prom / both
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/soft-a1}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-5}"
N="${N:-50000}"

g++ -std=c++17 -O2 -g -pthread -Iinclude bench/bench4.cpp -o build/bench4 -lsqlite3

CSV="$OUT/latency_p2_tax.csv"
echo "round,arm,insert_us,update_us,get_us,range_us,max_commit_us,starve_fits,starve_promotes,merge_parts,hw_reclaims" > "$CSV"

run_one() {
  local arm="$1" sf="$2" sp="$3" round="$4"
  local log="$OUT/tax_r${round}_${arm}.log"
  rm -f /home/user/.fcs_bench.db /home/user/.sqlite_bench.db \
        /home/user/.sqlite_bench.db-wal /home/user/.sqlite_bench.db-shm 2>/dev/null || true
  # Soft sync: soft=1 sp_accel=0 bf=0 skip=0 sf sp
  ./build/bench4 "$N" 500 32 "$N" 2000 100 0 0 0 0 0 0 0 1 0 0 1 0 0 0 "$sf" "$sp" \
    > "$log" 2>&1
  local ins upd get rng cmax fits prom parts hwr
  ins=$(awk '/^fcs insert/{print $3; exit}' "$log")
  upd=$(awk '/^fcs update/{print $3; exit}' "$log")
  get=$(awk '/^fcs get/{print $3; exit}' "$log")
  rng=$(awk '/^fcs range /{print $3; exit}' "$log")
  cmax=$(awk '/max commit =/{for(i=1;i<=NF;i++) if($i=="="){print $(i+1); exit}}' "$log")
  fits=$(grep -oP 'starve_fits=\K[0-9]+' "$log" | tail -1 || echo 0)
  prom=$(grep -oP 'starve_promotes=\K[0-9]+' "$log" | tail -1 || echo 0)
  parts=$(grep -oP 'merge_parts=\K[0-9]+' "$log" | tail -1 || echo 0)
  hwr=$(grep -oP 'hw_reclaims=\K[0-9]+' "$log" | tail -1 || echo 0)
  echo "$round,$arm,$ins,$upd,$get,$rng,${cmax:-},$fits,$prom,$parts,$hwr" >> "$CSV"
  echo "  r$round $arm ins=$ins upd=$upd get=$get fits=$fits prom=$prom parts=$parts"
}

echo "== Soft sync write-tax ablate N=$N rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  run_one off 0 0 "$r"
  run_one fit 1 0 "$r"
  run_one prom 0 1 "$r"
  run_one both 1 1 "$r"
done

python3 - <<'PY' "$CSV" "$OUT/VERDICT_P2_TAX.md"
import statistics as st, sys
csv, md = sys.argv[1], sys.argv[2]
rows = []
with open(csv, encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        try:
            rows.append({
                "arm": d["arm"],
                "insert": float(d["insert_us"]),
                "update": float(d["update_us"]),
                "get": float(d["get_us"]),
                "range": float(d["range_us"]),
                "cmax": float(d["max_commit_us"] or "nan"),
                "fits": float(d["starve_fits"] or 0),
                "prom": float(d["starve_promotes"] or 0),
                "parts": float(d["merge_parts"] or 0),
            })
        except ValueError:
            continue

def med(xs):
    xs = [x for x in xs if x == x]
    return st.median(xs) if xs else float("nan")

arms = ["off", "fit", "prom", "both"]
stats = {}
for a in arms:
    sub = [r for r in rows if r["arm"] == a]
    stats[a] = {k: med([r[k] for r in sub]) for k in ("insert", "update", "get", "range", "cmax", "fits", "prom", "parts")}

off = stats["off"]
lines = [
    "# P2 写税归因（Soft sync 交错消融）",
    "",
    f"源：`{csv}` · median µs/op · N=50k Soft sync",
    "",
    "| arm | insert | Δ% | update | Δ% | get | Δ% | fits | prom | parts |",
    "|-----|-------:|---:|-------:|---:|----:|---:|-----:|-----:|------:|",
]
for a in arms:
    s = stats[a]
    def dlt(k):
        o, v = off[k], s[k]
        return (v / o - 1) * 100 if o and o == o and v == v else float("nan")
    lines.append(
        f"| {a} | {s['insert']:.3f} | {dlt('insert'):+.1f}% | {s['update']:.3f} | {dlt('update'):+.1f}% | "
        f"{s['get']:.3f} | {dlt('get'):+.1f}% | {s['fits']:.0f} | {s['prom']:.0f} | {s['parts']:.0f} |"
    )

# attribution
def tax(arm, k):
    return (stats[arm][k] / off[k] - 1) * 100

lines += [
    "",
    "## 归因（相对 off）",
    "",
    f"- **fit 单开**：insert {tax('fit','insert'):+.1f}% · update {tax('fit','update'):+.1f}% · fits≈{stats['fit']['fits']:.0f} prom≈{stats['fit']['prom']:.0f}",
    f"- **prom 单开**：insert {tax('prom','insert'):+.1f}% · update {tax('prom','update'):+.1f}% · fits≈{stats['prom']['fits']:.0f} prom≈{stats['prom']['prom']:.0f}",
    f"- **both**：insert {tax('both','insert'):+.1f}% · update {tax('both','update'):+.1f}% · fits≈{stats['both']['fits']:.0f} prom≈{stats['both']['prom']:.0f}",
    "",
]
# leading source
ins = {a: tax(a, "insert") for a in ("fit", "prom", "both")}
upd = {a: tax(a, "update") for a in ("fit", "prom", "both")}
# which arm explains both
lines.append("## leading")
lines.append("")
if abs(ins["prom"]) >= abs(ins["fit"]) - 2 and stats["both"]["prom"] > 0:
    lines.append("- **写税主因倾向于 β `starve_promote`（额外 `commit_delta`/fsync）**：prom 臂有税且 both.prom>0。")
elif abs(ins["fit"]) > abs(ins["prom"]) + 3:
    lines.append("- **写税主因倾向于 α `starve_fit`（多片/多 run）**：fit 臂税显著、prom 臂弱。")
else:
    lines.append("- **写税为 α+β 合成**（或噪声）：看 fits/prom/parts 与 both 是否叠加。")
if stats["prom"]["prom"] == 0 and stats["both"]["prom"] > 0:
    lines.append("- prom 单开 triggers=0、both>0 ⇒ β 依附 α 分片（与 Soft A1 峰消融一致）。")
lines.append("")
open(md, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo P2_WRITE_TAX_ABLATE_OK
