#!/bin/bash
# bench/eval.sh —— 数据库内核全量评估矩阵（第 12 轮收尾）。
#   bash bench/eval.sh [section ...]          默认跑全部段；OUT=日志路径（默认 /tmp/eval.log）；REPS=重复次数（默认 3）
# 段：build amp sqlite bigscan mt rget snapmaint gc space ovw wprof stall mount storm robust meta
# 规则：每条命令原样写进日志（可直接复制复现）；带 timeout；噪声大的项重复 REPS 次（取中位由读者/summarize 做）；
#       计时段**不要**和 make test / 编译并发跑（2 核沙箱会把数字放大 1.5 ~ 3×）。
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/eval.log}
REPS=${REPS:-3}
CXX="g++ -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread -Iinclude"
want() { [ $# -eq 0 ] && return 0; for s in "${SECTIONS[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
SECTIONS=("$@")
log() { echo "$@" | tee -a "$OUT"; }
quiet() { sync; sleep ${QUIET:-5}; }   # 延迟敏感段之前：把前面基准留下的脏页写完（否则 fsync 排队，stall 的单次 get max 从 13 ms 变 70 ms）
run() {  # run <timeout_s> <cmd...>：命令行入日志，stdout+stderr 入日志，超时/失败标注
    local to=$1; shift
    log "\$ $*"
    local t0=$(date +%s.%N)
    timeout "$to" "$@" >>"$OUT" 2>&1; local rc=$?
    local t1=$(date +%s.%N)
    log "  -> rc=$rc wall=$(python3 -c "print(round($t1-$t0,2))") s"
}
build() {  # build <bin> <src> [extra flags]
    local bin=build/$1 src=$2; shift 2
    if [ -x "$bin" ] && [ "$bin" -nt "$src" ] && [ "$bin" -nt include/fcs.h ] && [ "$bin" -nt include/blockdev.h ]; then return 0; fi
    log "\$ $CXX $src $* -o $bin"
    $CXX "$src" "$@" -o "$bin" 2>&1 | grep -E "error|Error" | head -5 | tee -a "$OUT"   # 库（-lsqlite3）必须在源文件之后
}
mkdir -p build
log "===== eval start $(date -u +%FT%TZ) host=$(hostname) nproc=$(nproc) REPS=$REPS ====="
log "fcs.h lines=$(wc -l < include/fcs.h) md5=$(md5sum include/fcs.h | cut -c1-8)"

if want build; then
    log "### build"
    build bench3r bench/bench3.cpp
    build bench4 bench/bench4.cpp -lsqlite3
    build bench5 bench/bench5.cpp
    build bench6 bench/bench6.cpp
    build wprof bench/probe/wprof.cpp
    build wprof_lt bench/probe/wprof_lt.cpp
    build stall bench/probe/stall.cpp
    build mount bench/probe/mount.cpp
    build storm bench/probe/storm.cpp
    build h1 tests/h1.cpp
    build peakrss bench/probe/peakrss.cpp
    build g11 tests/g11.cpp
fi

if want amp; then
    log "### amp：写放大矩阵（RamDev，bench3r <ops> <commit_every> [group] [lvl_t]；CSV 末两列前是 write_bytes）"
    for a in "20000 500" "50000 1000" "200000 2000" "20000 100 8" "200000 2000 8" "200000 2000 0 4"; do
        run 300 build/bench3r $a
    done
fi

if want sqlite; then
    log "### sqlite：bench4 同负载正面对比（N B VSZ R S K kidx async group lvl_t async_min cons）"
    for i in $(seq $REPS); do
        run 600 build/bench4 50000 500 32 50000 2000 100                  # 同步提交，默认旋钮
        run 600 build/bench4 50000 500 32 50000 2000 100 0 1 1 8 0 1 1    # README 的「异步 + 组提交 + 归一」配置
    done
    run 900 build/bench4 200000 2000 100 100000 2000 100                 # 大一号（4× 键、100 B 值）
fi

if want bigscan; then
    log "### bigscan：大值（A2）与扫描预读（B2）"
    run 300 build/bench5 big 3001 4000
    run 300 build/bench5 big 8192 2000
    run 300 build/bench5 scan 128 20000 1
    run 300 build/bench5 scan 128 20000 0
    run 300 build/bench6 bigget 16 20000
    run 300 build/bench6 bigget 64 20000
fi

if want mt; then
    log "### mt：多写者（B3）"
    for i in $(seq $REPS); do
        run 300 build/bench5 mt 1 20000
        run 300 build/bench5 mt 4 5000
    done
fi

if want rget; then
    quiet
    log "### rget：读并发（共享读 / 独占；全命中 / 小缓存）"
    for i in $(seq $REPS); do
        run 300 build/peakrss build/bench6 rget 1 100000
        run 300 build/peakrss build/bench6 rget 2 100000
        run 300 build/peakrss build/bench6 rget 4 100000
        run 300 build/bench6 rget 2 100000 4096 0
    done
fi

if want snapmaint; then
    log "### snapmaint：快照期维护（P5）"
    run 300 build/bench6 snapmaint 2000 300 1
fi

if want gc; then
    log "### gc：长删循环 / 覆盖型负载的高水位"
    run 300 build/bench6 delloop 4 100000
    run 300 build/bench6 cfcurve 20000 3900 3
fi

if want ovw; then
    log "### ovw：覆盖型负载（R0 悬崖刻度；每 50 次提交一行 runs/cells/层级）"
    run 600 build/peakrss build/bench6 ovw 20000 500 200 500 50
fi

if want wprof; then
    quiet
    log "### wprof：10 万键 × 500 B，150 × 2000 覆盖提交（FileDev；avg/p50/p90/max，写放大）"
    for i in $(seq $REPS); do run 600 build/peakrss build/wprof_lt 100000 500 150 2000 8; done
    run 600 build/wprof_lt 100000 500 150 2000 4
    run 600 build/wprof_lt 100000 500 150 2000 1000
fi

if want stall; then
    quiet
    log "### stall：写者满负荷时读者 get 延迟（-1 写者空闲 / 0 默认维护 / 20 每 20 次提交 consolidate）"
    for i in $(seq $REPS); do run 120 build/stall 100000 100 4 0; done
    run 120 build/stall 100000 100 4 -1
    run 120 build/stall 100000 100 4 20
fi

if want mount; then
    quiet
    log "### mount：100 万条 × 50 B 的挂载时间 / 首批 get / RSS（lazy=1 默认，0 老行为）"
    run 900 build/mount 1000000 50 50000 3 1
    run 900 build/mount 1000000 50 50000 3 0
fi

if want storm; then
    log "### storm：12 工况 × TA × 2 种子（每提交逐键全量比对 + 快照视图；STORM_MAX_FAILS=5）"
    fails=0; n=0
    for base in r rd rdv rsb rsboc rsbocal rdvsc rc rsbl rdva rsbca rdvsbocal; do
        for seed in 1 2; do
            n=$((n+1))
            out=$(STORM_MAX_FAILS=5 timeout 300 build/storm ${base}TA $seed 3000 120 200 2>&1); rc=$?
            [ $rc -ne 0 ] && { fails=$((fails+1)); log "FAIL rc=$rc ${base}TA seed=$seed"; echo "$out" | tail -5 >>"$OUT"; }
            echo "$out" | grep -E "^storm\[" | tail -1 >>"$OUT"
        done
    done
    log "storm DONE runs=$n fails=$fails"
fi

if want robust; then
    log "### robust：挂载模糊 h1（400 次随机翻字节）+ g11 第 12 轮回归"
    run 300 build/h1 400 1
    run 300 build/g11
fi

if want meta; then
    log "### meta：代码体量 / 测试段数 / 公开 API 数"
    log "$(wc -l include/*.h tests/*.cpp bench/*.cpp bench/probe/*.cpp | tail -1)"
    log "fcs.h $(wc -l < include/fcs.h) 行；tests $(ls tests/*.cpp | wc -l) 个源；make test 段数 $(grep -c '^\s*@echo "== ' Makefile)"
    log "public API（Store 类 public 区的成员函数签名数）：$(python3 - <<'EOF'
import re
s=open('include/fcs.h').read()
i=s.find('class Store'); body=s[i:]
pub=False; n=0; names=set()
for line in body.splitlines():
    t=line.strip()
    if t.startswith('public:'): pub=True; continue
    if t.startswith('private:') or t.startswith('protected:'): pub=False; continue
    if pub:
        m=re.match(r'(?:static\s+)?(?:RC|void|bool|u32|u64|int|size_t|Vers|const\s+\w+[&*]?|std::\w+(?:<[^>]*>)?|\w+)\s+([a-z_][a-z0-9_]*)\s*\(', t)
        if m and not t.startswith('//') and not t.startswith('return'): names.add(m.group(1))
print(len(names))
EOF
)"
fi
log "===== eval end $(date -u +%FT%TZ) ====="
