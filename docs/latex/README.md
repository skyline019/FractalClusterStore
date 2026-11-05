# FCS 技术文档（XeLaTeX）

对齐公开标签 **v0.2.0**（Soft 默认 Post · P2 Soft-on · peers-post）+ **坑 169 拆头**（2026-11-05）。

## 依赖（WSL）

- TeX Live：`xelatex`
- 中文字体：`Noto Serif CJK SC` / `Noto Sans CJK SC`

```bash
sudo apt-get install -y texlive-xetex texlive-lang-chinese fonts-noto-cjk fonts-dejavu
```

## 编译

```bash
cd embdb/x/docs/latex
make
# 产物：build/main.pdf
```

## 结构

| 文件 | 内容 |
|---|---|
| `main.tex` | 导言区与总装 |
| `chapters/01-overview` | 概述（含 v0.2.0 一页结论） |
| `chapters/11-code-deep` | **代码详解**（自底向上） |
| `chapters/02`--`05` | 架构 → 格式 → 路径 → 合并/P2 Soft-on |
| `chapters/06-fsync-tidy` | fsync 出锁 · SoftCons::Post · repay |
| `chapters/07-evaluation` | peers-post 对标 · 峰值刀 |
| `chapters/08-engineering` | CONSTRAINTS · PIT · Strict CI · 头文件布局（169） |
| `chapters/09`--`10` | 图示 · 踩坑（含 150--154、169） |
| `chapters/A-api` / `B-repro` | API · 复现 |

Markdown 正文分类见上一级 [`../README.md`](../README.md)。
