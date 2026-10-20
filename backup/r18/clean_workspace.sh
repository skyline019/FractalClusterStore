#!/bin/bash
# 剔除无用项进 backup，保留可构建产品树。可逆：见 CLEAN_MANIFEST.md
set -euo pipefail
X=/mnt/e/FCS/embdb/x
E=/mnt/e/FCS/embdb
F=/mnt/e/FCS
DEST="$X/backup/r18/swept"
STAMP=$(date +%Y%m%d)
mkdir -p "$DEST"/{docs-hist,bench-probe,embdb-parent,root-loose}

manifest="$DEST/CLEAN_MANIFEST.md"
{
  echo "# 工作区清扫 $STAMP"
  echo
  echo "还原：把对应项从本目录移回原路径（见下方清单）。"
  echo "\`build/\` 未归档（可 \`make\` 重建）；当时约 1.5G 二进制。"
  echo
  echo "## 已移入"
} > "$manifest"

move() {
  local src=$1 dest=$2
  if [ ! -e "$src" ]; then
    echo "skip missing: $src"
    return 0
  fi
  mkdir -p "$(dirname "$dest")"
  mv "$src" "$dest"
  echo "- \`$src\` → \`${dest#$X/}\`" >> "$manifest"
  echo "moved: $src"
}

# --- embdb/x ---
# 1) 可重建产物
if [ -d "$X/build" ]; then
  echo "removing regenerable build/ ($(du -sh "$X/build" | cut -f1))"
  rm -rf "$X/build"
  echo "- \`embdb/x/build/\` **deleted** (regenerable via make)" >> "$manifest"
fi

# 2) 空 tools/
if [ -d "$X/tools" ] && [ -z "$(ls -A "$X/tools")" ]; then
  rmdir "$X/tools"
  echo "- \`embdb/x/tools/\` **removed** (empty)" >> "$manifest"
fi

# 3) 历史讨论 / 分叉评估（产品 docs 留 DESIGN*/EVAL17/BATTLE/ROUND13-18/P2/DISCUSS15）
for f in DISCUSS10.md DISCUSS11.md DISCUSS12.md DISCUSS12_R3.md \
         EVAL12.md EVAL14_L_alt.md ROUND16_L_alt.md; do
  move "$X/docs/$f" "$DEST/docs-hist/$f"
done

# 4) 探针：保留 layout.cpp + README（R18 养洞仍用）；其余进 backup
mkdir -p "$X/bench/probe"
for f in "$X"/bench/probe/*; do
  base=$(basename "$f")
  case "$base" in
    layout.cpp|README.md) continue ;;
    *) move "$f" "$DEST/bench-probe/$base" ;;
  esac
done
# 指针 README 片段
if [ ! -f "$X/bench/probe/ARCHIVE.txt" ]; then
  cat > "$X/bench/probe/ARCHIVE.txt" <<'EOF'
其余历史探针（wprof / tomb_* / stall / …）已移至：
  backup/r18/swept/bench-probe/
本目录仅留 layout.cpp（ROUND16–18 盘面峰值探针）与 README。
EOF
fi

# 5) peers 复现脚本贴近日志
if [ -f "$X/bench/run_peers_r17.sh" ]; then
  move "$X/bench/run_peers_r17.sh" "$X/backup/peers-r17/run_peers_r17.sh"
fi

# --- embdb/ 父树：已升格残留 ---
move "$E/littlefs-master.zip" "$DEST/embdb-parent/littlefs-master.zip"
move "$E/new" "$DEST/embdb-parent/new"
# v2 残留薄目录（产品在 x/）
for d in include src tests bench; do
  if [ -e "$E/$d" ]; then
    move "$E/$d" "$DEST/embdb-parent/v2-$d"
  fi
done

# --- FCS 根松散垃圾 ---
move "$F/cg.txt" "$DEST/root-loose/cg.txt"

{
  echo
  echo "## 产品树保留"
  echo "- Makefile README.md include/ tests/ docs/{DESIGN*,EVAL17,BATTLE,ROUND13-18,P2_DESIGN,DISCUSS15_FSYNC}"
  echo "- bench/{bench2-7,ab.sh,eval.sh,probe/layout.cpp}"
  echo "- web/ + 根目录 fcs-anim.html / fcs-pits.html（可视化交付物）"
  echo "- backup/（历史轮次与本清扫）"
} >> "$manifest"

echo "DONE"
ls -la "$X"
echo "---"
cat "$manifest"
