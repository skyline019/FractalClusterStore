#!/usr/bin/env bash
# 坑 156 S7：死账出独占（共享锁 settle）+ syncstall
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/concur-imm}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-3}"

g++ -std=c++17 -O2 -g -pthread -Iinclude bench/dual_probe.cpp -o build/dual_probe

CSV="$OUT/dual_s7.csv"
echo "round,mode,arm,wall_us,insert_us_op,max_commit_us,flush_excl_us,encode_us,io_us,settle_us,flushes,get_max_us,peak_tail,peak_x" > "$CSV"

run_one() {
  local mode="$1" dual="$2" round="$3"; shift 3
  local arm=$([[ "$dual" == 1 ]] && echo on || echo off)
  local log="$OUT/s7_${mode}_r${round}_${arm}.log"
  ./build/dual_probe "$mode" "$dual" "$@" | tee "$log"
  local line; line=$(grep '^mode=' "$log" | tail -1)
  python3 - "$line" "$round" "$CSV" <<'PY'
import sys
line, rnd, csv = sys.argv[1], sys.argv[2], sys.argv[3]
d = dict(x.split("=", 1) for x in line.split())
keys = ["mode","arm","wall_us","insert_us_op","max_commit_us","flush_excl_us",
        "encode_us","io_us","settle_us","flushes","get_max_us","peak_tail","peak_x"]
open(csv, "a", encoding="utf-8").write(",".join([rnd]+[d.get(k,"0") for k in keys])+"\n")
print("  r%s %s %s wall=%s excl=%s settle=%s get_max=%s" % (
    rnd, d.get("mode"), d.get("arm"), d.get("wall_us"), d.get("flush_excl_us"),
    d.get("settle_us"), d.get("get_max_us")))
PY
}

echo "== dual_mem S7 ablate rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  run_one write 0 "$r" 50000 200 128
  run_one write 1 "$r" 50000 200 128
  run_one lock  0 "$r" 20000 500 512
  run_one lock  1 "$r" 20000 500 512
  run_one stall 0 "$r" 8000 400 256
  run_one stall 1 "$r" 8000 400 256
  run_one mw    0 "$r" 15000 200 256 3
  run_one mw    1 "$r" 15000 200 256 3
  run_one syncstall 0 "$r" 8 1 120
  run_one syncstall 1 "$r" 8 1 120
done

python3 - <<'PY' "$CSV" "$OUT/VERDICT_DUAL_MEM_S7.md"
import statistics as st, sys
csv, md = sys.argv[1], sys.argv[2]
rows=[]
with open(csv,encoding="utf-8") as f:
    hdr=f.readline().strip().split(",")
    for line in f:
        d=dict(zip(hdr,line.strip().split(",")))
        try:
            for k in hdr:
                if k in ("mode","arm"): continue
                d[k]=float(d[k])
            rows.append(d)
        except ValueError: pass

def med(mode,arm,key):
    xs=[r[key] for r in rows if r["mode"]==mode and r["arm"]==arm]
    return st.median(xs) if xs else float("nan")
def ratio(a,b):
    return a/b if a==a and b==b and b else float("nan")

modes=["write","lock","stall","mw","syncstall"]
S={m:{"off":{},"on":{}} for m in modes}
for m in modes:
  for arm in ("off","on"):
    for k in ("wall_us","flush_excl_us","encode_us","io_us","settle_us","flushes","get_max_us"):
      S[m][arm][k]=med(m,arm,k)

lock_ok = S["lock"]["off"]["flush_excl_us"]>0 and S["lock"]["on"]["flush_excl_us"]<=S["lock"]["off"]["flush_excl_us"]*0.5
write_ok = S["write"]["off"]["wall_us"]>0 and S["write"]["on"]["wall_us"]<=S["write"]["off"]["wall_us"]*1.10
# get_max：绝对门 <20ms；相对放宽到 1.5× 或 +500µs
def get_ok(mode):
    o,n=S[mode]["off"]["get_max_us"],S[mode]["on"]["get_max_us"]
    if not (o==o and n==n): return False
    return n<20000 and (n<=max(o*1.5, o+500) or n<2000)
stall_ok=get_ok("stall"); mw_get_ok=get_ok("mw"); sync_ok=get_ok("syncstall")
mw_wall_ok=S["mw"]["off"]["wall_us"]>0 and S["mw"]["on"]["wall_us"]<=S["mw"]["off"]["wall_us"]*1.10
# write excl should drop vs S6 if settle left excl
write_excl_ratio=ratio(S["write"]["on"]["flush_excl_us"],S["write"]["off"]["flush_excl_us"])

verdict="Negate-keep"
gates=dict(lock=lock_ok,write=write_ok,stall=stall_ok,mw_wall=mw_wall_ok,mw_get=mw_get_ok,sync=sync_ok)
if all(gates.values()):
    why="S7 主门全过（含 syncstall 绝对/放宽相对）；仍缺生产多负载长稳 ⇒ Soft 默关。"
else:
    why="门禁: "+", ".join(f"{k}={'PASS' if v else 'FAIL'}" for k,v in gates.items())

def fmt(x,nd=0):
    if x!=x: return "NA"
    return f"{x:.{nd}f}" if nd else f"{x:.0f}"

lines=["# VERDICT_DUAL_MEM_S7（坑 156 · settle 出独占）","","复现: `bash scripts/ablate_dual_mem_s7.sh`","",
"## 中位","",
"| 画像 | arm | wall | excl | enc | io | settle | get_max |",
"|------|-----|------|------|-----|----|--------|---------|"]
for m in modes:
  for arm in ("off","on"):
    a=S[m][arm]
    lines.append(f"| {m} | {arm} | {fmt(a['wall_us'])} | {fmt(a['flush_excl_us'])} | {fmt(a['encode_us'])} | {fmt(a['io_us'])} | {fmt(a['settle_us'])} | {fmt(a['get_max_us'])} |")
lo,ln=S["lock"]["off"]["flush_excl_us"],S["lock"]["on"]["flush_excl_us"]
lines += ["","## 门禁","",
f"| 门 | 实测 | 过？ |",
f"|----|------|------|",
f"| lock excl ≤0.5× | {fmt(ln)}/{fmt(lo)}={fmt(ratio(ln,lo),2)}× | {'PASS' if lock_ok else 'FAIL'} |",
f"| write wall ≤1.10× | {fmt(ratio(S['write']['on']['wall_us'],S['write']['off']['wall_us']),2)}× | {'PASS' if write_ok else 'FAIL'} |",
f"| write excl on/off | {fmt(write_excl_ratio,2)}× | (观测) |",
f"| stall get_max | off={fmt(S['stall']['off']['get_max_us'])} on={fmt(S['stall']['on']['get_max_us'])} | {'PASS' if stall_ok else 'FAIL'} |",
f"| mw wall | {fmt(ratio(S['mw']['on']['wall_us'],S['mw']['off']['wall_us']),2)}× | {'PASS' if mw_wall_ok else 'FAIL'} |",
f"| mw get_max | off={fmt(S['mw']['off']['get_max_us'])} on={fmt(S['mw']['on']['get_max_us'])} | {'PASS' if mw_get_ok else 'FAIL'} |",
f"| syncstall get_max | off={fmt(S['syncstall']['off']['get_max_us'])} on={fmt(S['syncstall']['on']['get_max_us'])} | {'PASS' if sync_ok else 'FAIL'} |",
"",f"## 判决：**{verdict}**","",why,
"","S7：XLock=切表+attach；共享锁+settle_mu_=probe_prev 死账；读者可与 settle 并行。"]
open(md,"w",encoding="utf-8").write("\n".join(lines)+"\n")
print("\n".join(lines))
PY
echo "Wrote $OUT/VERDICT_DUAL_MEM_S7.md"
