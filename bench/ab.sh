#!/bin/bash
# bench/ab.sh —— A/B 对比脚本（第 11 轮 H4）：两份 fcs.h（A = 改前，B = 改后）各编一份探针，交替运行 N 次，报中位数。
# 用法：bench/ab.sh <A_fcs.h> <B_fcs.h> <probe.cpp> [N=3] [-- probe args...]
#   例：bench/ab.sh backup/r11/fcs.h.post-step0 include/fcs.h bench/probe/wprof.cpp 3 -- 100000 500 150 2000
# 约定：
#   * A/B 两份头文件会被拷进临时 include 目录（其余头取自 include/），探针用 -O2 -g 编译到 /tmp/ab.<pid>/；
#   * 交替运行（A B A B …）抵消机器噪声；每次 run 只取探针**最后一行**输出，原样打印，再按行里的 "avg X" / "p50 X" / "max X"
#     （若存在）各算中位数；没有这些字段就只打印原始行，自己看；
#   * 每次 run 都包在 timeout 600 里；探针运行目录 = 临时目录（wprof 需要 build/，脚本会先 mkdir）。
#   * 改前的头文件请放在工作区 backup/rNN/ 下（/tmp 会随沙箱轮换丢失 —— 第 10 轮已经丢过一次）。
set -u
A=$1; B=$2; PROBE=$3; shift 3
N=3; if [ $# -gt 0 ] && [ "$1" != "--" ]; then N=$1; shift; fi
[ "${1:-}" = "--" ] && shift
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$A" ] || { echo "A 头文件不存在：$A"; exit 1; }      # 少了这一行会悄悄变成 A == B（第 11 轮踩过）
[ -f "$B" ] || { echo "B 头文件不存在：$B"; exit 1; }
T=/tmp/ab.$$; mkdir -p $T/incA $T/incB $T/build
cp $ROOT/include/*.h $T/incA/; cp $ROOT/include/*.h $T/incB/
cp "$A" $T/incA/fcs.h; cp "$B" $T/incB/fcs.h
g++ -std=c++17 -O2 -g -pthread -I$T/incA "$ROOT/$PROBE" -o $T/probeA || { echo "A 编译失败"; exit 1; }
g++ -std=c++17 -O2 -g -pthread -I$T/incB "$ROOT/$PROBE" -o $T/probeB || { echo "B 编译失败"; exit 1; }
cd $T
declare -a LA LB
for ((i = 0; i < N; i++)); do
  LA[$i]=$(timeout 600 ./probeA "$@" 2>/dev/null | tail -1); echo "A[$i]: ${LA[$i]}"
  LB[$i]=$(timeout 600 ./probeB "$@" 2>/dev/null | tail -1); echo "B[$i]: ${LB[$i]}"
done
med() { # med <field> lines...
  local f=$1; shift
  printf '%s\n' "$@" | grep -o "$f [0-9.]*" | awk '{print $2}' | sort -g | awk '{a[NR]=$1} END{if(NR) printf "%s", a[int((NR+1)/2)]}'
}
echo "---- 中位数（N=$N，交替运行）"
for f in avg p50 p90 max; do
  ma=$(med "$f" "${LA[@]}"); mb=$(med "$f" "${LB[@]}")
  [ -n "$ma" ] && [ -n "$mb" ] && awk -v f=$f -v a=$ma -v b=$mb 'BEGIN{printf "%-4s A=%s B=%s  (B/A=%.2f)\n", f, a, b, (a>0)?b/a:0}'
done
echo "临时目录：$T（可删）"
