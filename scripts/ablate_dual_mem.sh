#!/usr/bin/env bash
# 坑 156：dual_mem 完整消融（DISCUSS_CONCUR_IMM §6）
# 产出：backup/concur-imm/{dual_full.csv,VERDICT_DUAL_MEM.md}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-backup/concur-imm}"
mkdir -p "$OUT" build
ROUNDS="${ROUNDS:-3}"

echo "== build dual_probe =="
g++ -std=c++17 -O2 -g -pthread -Iinclude bench/dual_probe.cpp -o build/dual_probe

CSV="$OUT/dual_full.csv"
echo "round,mode,arm,wall_us,insert_us_op,max_commit_us,flush_excl_us,encode_us,flushes,get_max_us,peak_tail,peak_x" > "$CSV"

run_one() {
  local mode="$1" dual="$2" round="$3" n="$4" b="$5" vsz="$6"
  local arm=$([[ "$dual" == 1 ]] && echo on || echo off)
  local log="$OUT/${mode}_r${round}_${arm}.log"
  if ! ./build/dual_probe "$mode" "$dual" "$n" "$b" "$vsz" | tee "$log"; then
    echo "FAIL $mode arm=$arm round=$round" >&2
    return 1
  fi
  # parse key=val line
  local line
  line=$(grep '^mode=' "$log" | tail -1)
  python3 - "$line" "$round" "$CSV" <<'PY'
import sys
line, rnd, csv = sys.argv[1], sys.argv[2], sys.argv[3]
d = dict(x.split("=", 1) for x in line.split())
keys = ["mode","arm","wall_us","insert_us_op","max_commit_us","flush_excl_us",
        "encode_us","flushes","get_max_us","peak_tail","peak_x"]
row = [rnd] + [d.get(k, "") for k in keys]
open(csv, "a", encoding="utf-8").write(",".join(row) + "\n")
print("  r%s %s %s wall=%s excl=%s enc=%s get_max=%s peak_x=%s" % (
    rnd, d.get("mode"), d.get("arm"), d.get("wall_us"), d.get("flush_excl_us"),
    d.get("encode_us"), d.get("get_max_us"), d.get("peak_x")))
PY
}

echo "== dual_mem FULL ablate rounds=$ROUNDS =="
for r in $(seq 1 "$ROUNDS"); do
  # 写吞吐 Soft sync 50k
  run_one write 0 "$r" 50000 200 128
  run_one write 1 "$r" 50000 200 128
  # 锁内墙钟：大 value
  run_one lock  0 "$r" 20000 500 512
  run_one lock  1 "$r" 20000 500 512
  # 读者 max（写中 get）
  run_one stall 0 "$r" 8000 400 256
  run_one stall 1 "$r" 8000 400 256
  # Soft A1 峰（NOTE）
  run_one a1    0 "$r" 50000 500 32
  run_one a1    1 "$r" 50000 500 32
  # Soft async
  run_one async 0 "$r" 50000 200 128
  run_one async 1 "$r" 50000 200 128
done

python3 - <<'PY' "$CSV" "$OUT/VERDICT_DUAL_MEM.md"
import statistics as st, sys, math
csv, md = sys.argv[1], sys.argv[2]
rows = []
with open(csv, encoding="utf-8") as f:
    hdr = f.readline().strip().split(",")
    for line in f:
        parts = line.strip().split(",")
        if len(parts) != len(hdr):
            continue
        d = dict(zip(hdr, parts))
        try:
            for k in hdr:
                if k in ("mode", "arm"):
                    continue
                d[k] = float(d[k])
            rows.append(d)
        except ValueError:
            continue

def med(mode, arm, key):
    xs = [r[key] for r in rows if r["mode"] == mode and r["arm"] == arm]
    return st.median(xs) if xs else float("nan")

def ratio(a, b):
    if not (a == a and b == b) or b == 0:
        return float("nan")
    return a / b

modes = ["write", "lock", "stall", "a1", "async"]
stats = {}
for m in modes:
    stats[m] = {
        "off": {k: med(m, "off", k) for k in ("wall_us","insert_us_op","max_commit_us","flush_excl_us","encode_us","flushes","get_max_us","peak_tail","peak_x")},
        "on":  {k: med(m, "on",  k) for k in ("wall_us","insert_us_op","max_commit_us","flush_excl_us","encode_us","flushes","get_max_us","peak_tail","peak_x")},
    }

# Gates (DISCUSS §6)
lock_off = stats["lock"]["off"]["flush_excl_us"]
lock_on  = stats["lock"]["on"]["flush_excl_us"]
lock_ok = lock_off == lock_off and lock_on == lock_on and lock_off > 0 and lock_on <= lock_off * 0.5

write_off = stats["write"]["off"]["wall_us"]
write_on  = stats["write"]["on"]["wall_us"]
write_ok = write_off == write_off and write_on == write_on and write_off > 0 and write_on <= write_off * 1.10

stall_off = stats["stall"]["off"]["get_max_us"]
stall_on  = stats["stall"]["on"]["get_max_us"]
# 不劣于 off，且绝对 < 20ms
stall_ok = (stall_on == stall_on and stall_off == stall_off and
            stall_on <= max(stall_off * 1.05, stall_off + 100) and stall_on < 20000)

a1_off = stats["a1"]["off"]["peak_x"]
a1_on  = stats["a1"]["on"]["peak_x"]
a1_note = a1_on == a1_on and a1_off == a1_off and a1_off > 0 and a1_on > a1_off * 1.05

async_off = stats["async"]["off"]["wall_us"]
async_on  = stats["async"]["on"]["wall_us"]
async_ok = async_off == async_off and async_on == async_on and async_off > 0 and async_on <= async_off * 1.10

dual_fires = stats["lock"]["on"]["flushes"] > 0 and stats["write"]["on"]["flushes"] > 0
# async 路径设计上不双切
async_no_dual = stats["async"]["on"]["flushes"] == 0 or stats["async"]["on"]["flushes"] != stats["async"]["on"]["flushes"]

promote = lock_ok and write_ok and stall_ok and dual_fires and not a1_note
# 升默仍要多写者画像 — 本脚本无 ⇒ 默认 Negate-keep
verdict = "Negate-keep"
if not dual_fires:
    verdict = "Negate"
    why = "dual 路径 flushes 未触发。"
elif not write_ok:
    verdict = "Negate"
    why = "Soft sync 写墙钟回归 >10%。"
elif a1_note and not (lock_ok and stall_ok):
    verdict = "Negate"
    why = "空间峰抬 >5% 且延迟门未全过。"
else:
    why = ("延迟门部分/全部可议通过，但缺多写者+慢 sync 读者画像稳定性 ⇒ **不升 Soft 默**；代码保留可选。"
           if not promote else
           "单写画像门禁可议通过；仍缺多写者 stall 稳定胜 ⇒ Soft 默保持关（Negate-keep）。")
    verdict = "Negate-keep"

def fmt(x, nd=0):
    if x != x:
        return "NA"
    if nd:
        return f"{x:.{nd}f}"
    return f"{x:.0f}"

lines = [
    "# VERDICT_DUAL_MEM（坑 156 · 完整消融）",
    "",
    "复现：`bash scripts/ablate_dual_mem.sh` → `backup/concur-imm/dual_full.csv`",
    "",
    "## 中位数对照（off → on）",
    "",
    "| 画像 | wall µs | insert µs/op | max commit µs | flush_excl µs | encode µs | flushes | get_max µs | peak_x |",
    "|------|---------|--------------|---------------|---------------|-----------|---------|------------|--------|",
]
for m in modes:
    o, n = stats[m]["off"], stats[m]["on"]
    lines.append(
        f"| {m} off | {fmt(o['wall_us'])} | {fmt(o['insert_us_op'],1)} | {fmt(o['max_commit_us'])} | "
        f"{fmt(o['flush_excl_us'])} | {fmt(o['encode_us'])} | {fmt(o['flushes'])} | "
        f"{fmt(o['get_max_us'])} | {fmt(o['peak_x'],3)} |"
    )
    lines.append(
        f"| {m} on  | {fmt(n['wall_us'])} | {fmt(n['insert_us_op'],1)} | {fmt(n['max_commit_us'])} | "
        f"{fmt(n['flush_excl_us'])} | {fmt(n['encode_us'])} | {fmt(n['flushes'])} | "
        f"{fmt(n['get_max_us'])} | {fmt(n['peak_x'],3)} |"
    )

lines += [
    "",
    "## §6 门禁",
    "",
    f"| 门 | 草案 | 实测 | 过？ |",
    f"|----|------|------|------|",
    f"| 锁内墙钟（lock 大 value） | on excl ≤ 0.5× off | "
    f"{fmt(lock_on)} / {fmt(lock_off)} = {fmt(ratio(lock_on, lock_off), 2)}× | {'PASS' if lock_ok else 'FAIL'} |",
    f"| 写吞吐 Soft sync 50k | on wall ≤ 1.10× off | "
    f"{fmt(write_on)} / {fmt(write_off)} = {fmt(ratio(write_on, write_off), 2)}× | {'PASS' if write_ok else 'FAIL'} |",
    f"| 读者 get_max（stall） | on 不劣于 off 且 <20ms | "
    f"off={fmt(stall_off)} on={fmt(stall_on)} | {'PASS' if stall_ok else 'FAIL'} |",
    f"| Soft async 写 | on wall ≤ 1.10× off | "
    f"{fmt(async_on)} / {fmt(async_off)} = {fmt(ratio(async_on, async_off), 2)}× | {'PASS' if async_ok else 'FAIL'} |",
    f"| Soft A1 峰（NOTE） | on peak_x 抬 >5% 记 NOTE | "
    f"off={fmt(a1_off,3)} on={fmt(a1_on,3)} ({fmt(100*(ratio(a1_on,a1_off)-1),1)}%) | "
    f"{'NOTE抬峰' if a1_note else 'OK'} |",
    f"| dual 触发 | write/lock on flushes>0 | lock_flushes={fmt(stats['lock']['on']['flushes'])} | "
    f"{'PASS' if dual_fires else 'FAIL'} |",
    "",
    f"## 判决：**{verdict}**",
    "",
    why,
    "",
    "- Soft / `configure_deploy`：**不得**默认 `set_dual_mem(true)`。",
    "- async 路径设计上不走双切（`!async_`）；async 行 flushes≈0 属预期。",
    "- KPI_UNVERIFIED：多写者 + 慢 sync 窗口读者（g17 精神）未纳入本矩阵。",
    "",
    "关联：`DISCUSS_CONCUR_IMM.md` §6 · 坑 156",
]
text = "\n".join(lines) + "\n"
open(md, "w", encoding="utf-8").write(text)
print(text)
PY

echo "Wrote $OUT/VERDICT_DUAL_MEM.md"
