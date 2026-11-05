#!/usr/bin/env bash
# 坑 156 S6：finish_run 出锁 + 多写者画像（补全 §6）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/concur-imm}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-3}"

g++ -std=c++17 -O2 -g -pthread -Iinclude bench/dual_probe.cpp -o build/dual_probe

CSV="$OUT/dual_s6.csv"
echo "round,mode,arm,wall_us,insert_us_op,max_commit_us,flush_excl_us,encode_us,io_us,flushes,get_max_us,peak_tail,peak_x" > "$CSV"

run_one() {
  local mode="$1" dual="$2" round="$3"; shift 3
  local arm=$([[ "$dual" == 1 ]] && echo on || echo off)
  local log="$OUT/s6_${mode}_r${round}_${arm}.log"
  ./build/dual_probe "$mode" "$dual" "$@" | tee "$log"
  local line
  line=$(grep '^mode=' "$log" | tail -1)
  python3 - "$line" "$round" "$CSV" <<'PY'
import sys
line, rnd, csv = sys.argv[1], sys.argv[2], sys.argv[3]
d = dict(x.split("=", 1) for x in line.split())
keys = ["mode","arm","wall_us","insert_us_op","max_commit_us","flush_excl_us",
        "encode_us","io_us","flushes","get_max_us","peak_tail","peak_x"]
row = [rnd] + [d.get(k, "0") for k in keys]
open(csv, "a", encoding="utf-8").write(",".join(row) + "\n")
print("  r%s %s %s wall=%s excl=%s enc=%s io=%s get_max=%s" % (
    rnd, d.get("mode"), d.get("arm"), d.get("wall_us"), d.get("flush_excl_us"),
    d.get("encode_us"), d.get("io_us"), d.get("get_max_us")))
PY
}

echo "== dual_mem S6 ablate rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  run_one write 0 "$r" 50000 200 128
  run_one write 1 "$r" 50000 200 128
  run_one lock  0 "$r" 20000 500 512
  run_one lock  1 "$r" 20000 500 512
  run_one stall 0 "$r" 8000 400 256
  run_one stall 1 "$r" 8000 400 256
  run_one mw    0 "$r" 15000 200 256 3
  run_one mw    1 "$r" 15000 200 256 3
done

python3 - <<'PY' "$CSV" "$OUT/VERDICT_DUAL_MEM_S6.md"
import statistics as st, sys
csv, md = sys.argv[1], sys.argv[2]
rows = []
with open(csv, encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        d = dict(zip(hdr, line.strip().split(",")))
        try:
            for k in hdr:
                if k in ("mode", "arm"): continue
                d[k] = float(d[k])
            rows.append(d)
        except ValueError:
            continue

def med(mode, arm, key):
    xs = [r[key] for r in rows if r["mode"] == mode and r["arm"] == arm]
    return st.median(xs) if xs else float("nan")

def ratio(a, b):
    if not (a == a and b == b) or b == 0: return float("nan")
    return a / b

modes = ["write", "lock", "stall", "mw"]
S = {m: {"off": {}, "on": {}} for m in modes}
for m in modes:
    for arm in ("off", "on"):
        for k in ("wall_us","flush_excl_us","encode_us","io_us","flushes","get_max_us","max_commit_us"):
            S[m][arm][k] = med(m, arm, k)

lock_ok = S["lock"]["off"]["flush_excl_us"] > 0 and S["lock"]["on"]["flush_excl_us"] <= S["lock"]["off"]["flush_excl_us"] * 0.5
write_ok = S["write"]["off"]["wall_us"] > 0 and S["write"]["on"]["wall_us"] <= S["write"]["off"]["wall_us"] * 1.10
stall_ok = S["stall"]["on"]["get_max_us"] < 20000 and S["stall"]["on"]["get_max_us"] <= S["stall"]["off"]["get_max_us"] * 1.05 + 100
mw_wall_ok = S["mw"]["off"]["wall_us"] > 0 and S["mw"]["on"]["wall_us"] <= S["mw"]["off"]["wall_us"] * 1.10
mw_get_ok = S["mw"]["on"]["get_max_us"] < 20000 and (
    S["mw"]["on"]["get_max_us"] <= S["mw"]["off"]["get_max_us"] * 1.05 + 100)
dual_fires = S["lock"]["on"]["flushes"] > 0

verdict = "Negate-keep"
why = "S6 后仍默认不升 Soft。"
if not dual_fires:
    verdict, why = "Negate", "dual flushes 未触发。"
elif not write_ok or not mw_wall_ok:
    verdict, why = "Negate", "写墙钟回归 >10%。"
elif lock_ok and write_ok and stall_ok and mw_wall_ok and mw_get_ok:
    verdict = "Negate-keep"
    why = ("锁内≤0.5× 且多写者 wall/get 可议通过，但仍缺慢 sync 窗口稳定画像；"
           "Soft 默保持关，代码保留可选。")
else:
    why = (f"lock_ok={lock_ok} write_ok={write_ok} stall_ok={stall_ok} "
           f"mw_wall_ok={mw_wall_ok} mw_get_ok={mw_get_ok}；不升 Soft。")

def fmt(x, nd=0):
    if x != x: return "NA"
    return f"{x:.{nd}f}" if nd else f"{x:.0f}"

lines = [
    "# VERDICT_DUAL_MEM_S6（坑 156 · finish 出锁 + 多写者）",
    "",
    "复现：`bash scripts/ablate_dual_mem_s6.sh`",
    "",
    "## 中位对照",
    "",
    "| 画像 | arm | wall | flush_excl | encode | io | flushes | get_max |",
    "|------|-----|------|------------|--------|----|---------|---------|",
]
for m in modes:
    for arm in ("off", "on"):
        a = S[m][arm]
        lines.append(
            f"| {m} | {arm} | {fmt(a['wall_us'])} | {fmt(a['flush_excl_us'])} | "
            f"{fmt(a['encode_us'])} | {fmt(a['io_us'])} | {fmt(a['flushes'])} | {fmt(a['get_max_us'])} |"
        )

lo, ln = S["lock"]["off"]["flush_excl_us"], S["lock"]["on"]["flush_excl_us"]
lines += [
    "",
    "## 门禁",
    "",
    f"| 门 | 实测 | 过？ |",
    f"|----|------|------|",
    f"| 锁内 excl ≤0.5× | {fmt(ln)}/{fmt(lo)}={fmt(ratio(ln,lo),2)}× | {'PASS' if lock_ok else 'FAIL'} |",
    f"| write wall ≤1.10× | {fmt(ratio(S['write']['on']['wall_us'], S['write']['off']['wall_us']),2)}× | {'PASS' if write_ok else 'FAIL'} |",
    f"| stall get_max | off={fmt(S['stall']['off']['get_max_us'])} on={fmt(S['stall']['on']['get_max_us'])} | {'PASS' if stall_ok else 'FAIL'} |",
    f"| mw×3 wall ≤1.10× | {fmt(ratio(S['mw']['on']['wall_us'], S['mw']['off']['wall_us']),2)}× | {'PASS' if mw_wall_ok else 'FAIL'} |",
    f"| mw get_max | off={fmt(S['mw']['off']['get_max_us'])} on={fmt(S['mw']['on']['get_max_us'])} | {'PASS' if mw_get_ok else 'FAIL'} |",
    "",
    f"## 判决：**{verdict}**",
    "",
    why,
    "",
    "S6 机制：锁外 encode + finish_run（`dual_mu_` 串行 free-list）；锁内仅 attach/账。",
    "Soft 默认仍关。",
]
open(md, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY
echo "Wrote $OUT/VERDICT_DUAL_MEM_S6.md"
