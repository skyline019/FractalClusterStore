#!/usr/bin/env python3
"""Comment-only modularization. No logic changes. Restore: backup/r18/pre-comment-modular/."""
from __future__ import annotations

from pathlib import Path

ROOT = Path("/mnt/e/FCS/embdb/x")
INC = ROOT / "include"
BAK = ROOT / "backup/r18/pre-comment-modular"
EQ = "// ============================================================================="


def top(mid: str, title: str, role: str = "") -> str:
    line = f"//  {mid}  {title}"
    if role:
        line += f"  —  {role}"
    return f"{EQ}\n{line}\n{EQ}"


def sub(mid: str, title: str) -> str:
    """Indented Store subsection (same visual weight as old ---- lines)."""
    return f"    // ------------------------------------------------------------ {mid}  {title}"


def once(text: str, old: str, new: str, label: str) -> str:
    """Replace a whole comment line; require unique match as a full line."""
    # Prefer line-anchored match so short titles are not prefixes of longer ones.
    if "\n" not in old.rstrip("\n"):
        needle = old.rstrip("\n")
        lines = text.splitlines(keepends=True)
        hits = [i for i, ln in enumerate(lines) if ln.rstrip("\r\n") == needle]
        if not hits:
            raise SystemExit(f"missing [{label}]: {old[:80]!r}")
        if len(hits) != 1:
            raise SystemExit(f"ambiguous [{label}]: count={len(hits)}")
        i = hits[0]
        eol = "\n" if lines[i].endswith("\n") else ""
        if lines[i].endswith("\r\n"):
            eol = "\r\n"
        lines[i] = new + eol
        return "".join(lines)
    n = text.count(old)
    if n == 0:
        raise SystemExit(f"missing [{label}]: {old[:80]!r}")
    if n != 1:
        raise SystemExit(f"ambiguous [{label}]: count={n}")
    return text.replace(old, new, 1)


def code_skel(s: str) -> list[str]:
    out = []
    for ln in s.splitlines():
        st = ln.strip()
        if not st or st.startswith("//"):
            continue
        out.append(ln)
    return out


def modularize_fcs(text: str) -> str:
    old_hdr = """\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
"""
    # Use exact file header from backup (lines 1-33)
    hdr_end = text.find("#pragma once")
    if hdr_end < 0:
        raise SystemExit("no #pragma once")
    old_hdr = text[:hdr_end]
    new_hdr = """\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
\
"""
    # Build properly without the broken empty strings above
    new_hdr = (
        "// ============================================================================\n"
        "//  fcs.h —— FCS: Fractal Cluster Store（XDB v3 内核）\n"
        "// ============================================================================\n"
        "//  命题：**「树」只是「平衡」的一种派生形态**。v2 的 B+ 树把大量复杂度花在\n"
        "//  「维持平衡」的副作用上：分隔键下推、父指针、级联分裂、根换位、回退时\n"
        "//  整树重建、每页 COW。\n"
        "//\n"
        "//  FCS 直接维护平衡本身，于是树消失了。三个同构（分形）层次：\n"
        "//\n"
        "//     store ── 有序「单元表 directory」 ── 单元 cell = 键区间 + 段视图\n"
        "//       run ── 有序「页表 fences」      ── 页 = 有序记录序列\n"
        "//      page ── 有序记录（页内二分/线性）\n"
        "//\n"
        "//  关键点：**单元不拥有数据**。数据只存在于不可变的段（run）里；单元只是\n"
        "//  「(段, 页区间)」的集合视图。因此\n"
        "//      · 分裂单元 = 把页区间一分为二  → 零数据重写，O(#段)\n"
        "//      · 合并单元 = 页区间取并        → 零数据重写，O(#段)\n"
        "//  平衡变成目录上的局部操作，不再自底向上传播。\n"
        "//\n"
        "//  三条不变量（平衡的定义，与树无关）：\n"
        "//    B1  单元物理字节 ≤ BMAX          —— 超了就在页边界切成两半\n"
        "//    B2  单元段数 ≤ SMAX              —— 超了就压实成一个段\n"
        "//    B3  每次提交的重写预算随垃圾产生速率自适应，0/1 背包按“净收益”分配\n"
        "//\n"
        "//  动态规划：把候选压实单元当作物品（成本 = 重写页数，收益 = 回收字节 +\n"
        "//  减少段数），在预算内求最优子集 —— 压实时机不是启发式。\n"
        "//\n"
        "//  写：mem → **每提交 1 个有序段**（顺序写，与单元数无关）→ 清单增量页\n"
        "//      → 4B 提交字。写放大 ≈ 1 + 记录头 + 压实摊销；无整页 COW。\n"
        "//  读：目录二分 O(log C) → 段按版本**从新到旧**探测，命中即停 → 通常 1 页。\n"
        "//  回退：快照 = {vers, tail, man_last, nman}；回退 = 指针回退 + 清单截断，\n"
        "//        **O(1)**，零数据页重写（快照存活期间页不回收）。\n"
        "//\n"
        "//  【模块 TOC】（与下方 Mxx 横幅一一对应；仅注释地图，无逻辑边界）\n"
        "//    M00  本导读\n"
        "//    M01  参数（容量 / 层 / 特性宏）\n"
        "//    M02  编解码（记录头 / 页内前缀）\n"
        "//    M03  内存表 MemTable\n"
        "//    M04  布隆过滤器\n"
        "//    M05  段 / 单元（Run · Seg · Cell）\n"
        "//    M06  线程槽 / 读写锁（P1）\n"
        "//    M07  Store（公开面 + 实现；子节 M07a…）\n"
        "//  修坏还原：backup/r18/pre-comment-modular/\n"
        "// ============================================================================\n"
    )
    text = new_hdr + text[hdr_end:]

    tops = [
        ("// ---------------------------------------------------------------- 参数", top("M01", "参数", "容量 / 层 / MemTable / 压缩触发 / 特性宏")),
        ("// ---------------------------------------------------------------- 编解码", top("M02", "编解码", "记录头 · 页内前缀 · CRC")),
        ("// ------------------------------------------------------------ 内存表", top("M03", "内存表", "MemTable：点查 / 覆盖写 / 有序吐出")),
        ("// ---------------------------------------------------------------- 布隆过滤器", top("M04", "布隆过滤器", "段级假阳性过滤")),
        ("// ---------------------------------------------------------------- 段 / 单元", top("M05", "段 / 单元", "Run · Seg · Cell · Report")),
        ("// ---------------------------------------------------------------- 线程槽 / 读写锁（P1 读扩展性）",
         top("M06", "线程槽 / 读写锁", "P1 读扩展性：TlsId · XLock")),
    ]
    for old, new in tops:
        text = once(text, old, new, old)

    # Store class entry
    text = once(
        text,
        "class Store {",
        top("M07", "Store", "公开 API + 打开/写读/Flush/合并/恢复/整理态") + "\nclass Store {",
        "M07-class",
    )

    store_subs = [
        ("    // ------------------------------------------------------------ 线程私有状态（P1）", sub("M07a", "线程私有状态（P1）")),
        ("    // ------------------------------------------------------------ 段游标", sub("M07b", "段游标")),
        ("    // ------------------------------------------------------------ 异步写", sub("M07c", "异步写")),
        ("    // ---- 常开追踪环（A4）----", "    // --- M07d  常开追踪环（A4） ---"),
        ("    // ------------------------------------------------------------ 读", sub("M07e", "读")),
        ("    // ------------------------------------------------------------ 游标", sub("M07f", "游标")),
        ("    // ------------------------------------------------------------ 二级索引", sub("M07g", "二级索引")),
        ("    // ------------------------------------------------------------ 写", sub("M07h", "写")),
        ("    // ---- mode 3：后台维护线程 ----", "    // --- M07i  mode 3：后台维护线程 ---"),
        ("    // ------------------------------------------------------------ 快照 / 回退", sub("M07j", "快照 / 回退")),
        ("    // ------------------------------------------------------------ 校验", sub("M07k", "校验")),
        ("    // ------------------------------------------------------------ 清单项", sub("M07l", "清单项")),
        ("    // ------------------------------------------------------------ 只读页缓存（两路组相联）", sub("M07m", "只读页缓存（两路组相联）")),
        ("    // ------------------------------------------------------------ 页缓存（两路组相联）", sub("M07n", "页缓存（两路组相联）")),
        ("    // ------------------------------------------------------------ 目录", sub("M07o", "目录")),
        ("    // ------------------------------------------------------------ 异步写的实现", sub("M07p", "异步写的实现")),
        ("    // ------------------------------------------------------------ 挂载", sub("M07q", "挂载")),
        ("    // ------------------------------------------------------------ 清单", sub("M07r", "清单")),
        ("    // ------------------------------------------------------------ 空闲空间", sub("M07s", "空闲空间")),
        ("    // ------------------------------------------------------------ 段写入", sub("M07t", "段写入")),
        ("    // ------------------------------------------------------------ run 元数据页", sub("M07u", "run 元数据页")),
        ("    // ------------------------------------------------------------ 溢出值", sub("M07v", "溢出值")),
        ("    // ------------------------------------------------------------ 溢出链小缓存", sub("M07w", "溢出链小缓存")),
        ("    // ------------------------------------------------------------ R12 流式产物（第 13 轮）", sub("M07x", "R12 流式产物（第 13 轮）")),
        ("    // ------------------------------------------------------------ 提交：mem → 1 段", sub("M07y", "提交：mem → 1 段")),
        ("    // ------------------------------------------------------------ 平衡", sub("M07z", "平衡")),
        ("    // ---- R6-lite：惰性存活账 ----", "    // --- M07aa  R6-lite：惰性存活账 ---"),
        ("    // ------------------------------------------------------------ 分层合并", sub("M07ab", "分层合并")),
        ("    // ------------------------------------------------------------ 清单提交", sub("M07ac", "清单提交")),
        ("    // ------------------------------------------------------------ 读路径完整性", sub("M07ad", "读路径完整性")),
        ("    // ------------------------------------------------------------ 状态", sub("M07ae", "状态")),
        ("    // ---- 多写者（同一进程内）", "    // --- M07af  多写者（同一进程内） ---"),
    ]
    for old, new in store_subs:
        text = once(text, old, new, old.strip())

    # deploy / tidy cluster near configure_deploy
    text = once(
        text,
        "    // 部署整理（小入口，见 docs/EVAL17.md §3 / README「部署」）：",
        "    // --- M07h2  部署整理 / 常驻整理态（configure_deploy · polish · tidy） ---\n"
        "    // 部署整理（小入口，见 docs/EVAL17.md §3 / README「部署」）：",
        "deploy-tidy",
    )
    return text


def light_sibling(name: str, mid: str, title: str, role: str, section_map: list[tuple[str, str]]) -> None:
    path = INC / name
    raw = path.read_text(encoding="utf-8")
    bak = (BAK / name).read_text(encoding="utf-8")
    if raw != bak:
        raise SystemExit(f"{name} diverged from backup")
    lines = raw.splitlines(keepends=True)
    # Find end of leading // comment block
    i = 0
    while i < len(lines) and lines[i].startswith("//"):
        i += 1
    while i < len(lines) and lines[i].strip() == "":
        i += 1
    # Rewrite first-line title to modular banner; keep remaining prose of header
    # Simpler: insert TOC line after first comment line, and upgrade ---- sections
    text = raw
    # Upgrade section markers
    for old, lab in section_map:
        new = f"// ------------------------------------------------------------------ {mid}.{lab}"
        # keep Chinese title: old is full line like "// ------------------------------------------------------------------ 配置"
        # replace prefix dashes style only by tagging mid
        title_part = old.split(" ", 1)[-1] if " " in old else old
        # old format: "// ------------------------------------------------------------------ 配置"
        parts = old.split(None, 1)
        if len(parts) == 2:
            new = f"// ------------------------------------------------------------------ {mid}  {parts[1]}"
        else:
            new = old
        text = once(text, old, new, f"{name}:{old}")

    # Prepend module banner after existing one-line / multi-line header comments:
    # insert right before #pragma once
    pragma = text.find("#pragma once")
    if pragma < 0:
        raise SystemExit(f"{name}: no pragma")
    banner = top(mid, title, role) + "\n"
    if EQ in text[:pragma]:
        print(f"{name}: already has EQ banner, skip insert")
    else:
        text = text[:pragma] + banner + text[pragma:]

    if code_skel(raw) != code_skel(text):
        a, b = code_skel(raw), code_skel(text)
        for j, (x, y) in enumerate(zip(a, b)):
            if x != y:
                raise SystemExit(f"{name} CODE DRIFT @ {j}:\n  {x}\n  {y}")
        raise SystemExit(f"{name} CODE DRIFT len {len(a)}->{len(b)}")
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"OK {name}: {len(raw.splitlines())} -> {len(text.splitlines())}")


def main() -> None:
    fcs = INC / "fcs.h"
    raw = fcs.read_text(encoding="utf-8")
    bak = (BAK / "fcs.h").read_text(encoding="utf-8")
    if raw != bak:
        raise SystemExit("fcs.h != backup; refuse")
    out = modularize_fcs(raw)
    if code_skel(raw) != code_skel(out):
        a, b = code_skel(raw), code_skel(out)
        for j, (x, y) in enumerate(zip(a, b)):
            if x != y:
                raise SystemExit(f"fcs.h CODE DRIFT @ {j}:\n  {x}\n  {y}")
        raise SystemExit(f"fcs.h CODE DRIFT len {len(a)}->{len(b)}")
    fcs.write_text(out, encoding="utf-8", newline="\n")
    print(f"OK fcs.h: {len(raw.splitlines())} -> {len(out.splitlines())}")

    light_sibling(
        "common.h", "C00", "common", "类型 · 返回码 · 配置 · 哈希/CRC",
        [
            ("// ------------------------------------------------------------------ 配置", "配置"),
            ("// ------------------------------------------------------------------ 位运算", "位运算"),
            ("// ------------------------------------------------------------------ 哈希", "哈希"),
            ("// ------------------------------------------------------------------ CRC", "CRC"),
        ],
    )
    light_sibling(
        "blockdev.h", "D00", "blockdev", "块设备抽象 · 写放大计量",
        [
            ("// ------------------------------------------------------------------ RAM 设备", "RAM 设备"),
            ("// ------------------------------------------------------------------ 文件设备", "文件设备"),
            ("// ------------------------------------------------------------------ NOR 模拟", "NOR 模拟"),
        ],
    )
    light_sibling(
        "layout.h", "L00", "layout", "页面布局 · 叶/内/映射/锚",
        [
            ("// ------------------------------------------------------------------ 常量", "常量"),
            ("// ------------------------------------------------------------------ 基础读写字", "基础读写字"),
            ("// ------------------------------------------------------------------ 叶头", "叶头"),
            ("// ------------------------------------------------------------------ 槽访问", "槽访问"),
            ("// ------------------------------------------------------------------ 内页", "内页"),
            ("// ------------------------------------------------------------------ 映射页", "映射页"),
            ("// ------------------------------------------------------------------ 锚", "锚"),
        ],
    )
    light_sibling(
        "store.h", "S00", "store.h", "XDB v2 存储层（页缓存 / 叶操作 / 引擎）",
        [
            ("// ------------------------------------------------------------------ 引擎", "引擎"),
        ],
    )


if __name__ == "__main__":
    main()
