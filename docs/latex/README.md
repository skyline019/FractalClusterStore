# FCS 技术文档（XeLaTeX）

## 依赖（WSL）

- TeX Live：`xelatex`
- 中文字体：`Noto Serif CJK SC` / `Noto Sans CJK SC`（本机已有则无需安装）

```bash
sudo apt-get install -y texlive-xetex texlive-lang-chinese fonts-noto-cjk
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
| `chapters/01-overview` | 概述 |
| `chapters/11-code-deep` | **代码详解**（自底向上，面向基础 DB 读者；图 + 示例剧本） |
| `chapters/02`--`10` | 架构 → 格式 → 路径 → 合并 → fsync → 图示 → 踩坑 → 评估 → 工程 |
| `chapters/A-api` / `B-repro` | 附录 |

Markdown 正文分类见上一级 [`../README.md`](../README.md)（`00-entry` / `10-design` / `20-rounds` / `30-discuss`）。
