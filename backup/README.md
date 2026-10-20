# backup — 归档与实测日志分类

产品正文在 [`../docs/README.md`](../docs/README.md)。本目录只放**日志、快照、已扫走的历史文档**，勿当现行规格读。

## 对标 peers

| 目录 | 说明 |
|---|---|
| [`peers-soft/`](peers-soft/) | Soft / `configure_deploy` peers + 全量 `make test`（2026-10-21） |
| [`peers-r17/`](peers-r17/) | R17 基线旋钮 peers |
| [`wsl-20261020/`](wsl-20261020/) | 升格前 WSL 归档 |

## 轮次实测日志

| 目录 | 说明 |
|---|---|
| `r13/` … `r18/` | 各轮 layout / asan / 探针日志与脚本 |
| [`r18/swept/`](r18/swept/) | 工作区清扫产物（见 `CLEAN_MANIFEST.md`） |
| [`r18/swept/docs-hist/`](r18/swept/docs-hist/) | 旧 DISCUSS / EVAL12 / L 分叉评估等 |
| [`r18/pre-comment-modular/`](r18/pre-comment-modular/) | 注释模块化前快照 + `RESTORE.md` |
| [`r18/swept/bench-probe/`](r18/swept/bench-probe/) | 历史探针源码（`layout`/`storm` 仍留产品树） |

## 升格 / 分叉快照

| 目录 | 说明 |
|---|---|
| [`pre-battle-promote/`](pre-battle-promote/) | L 分叉树快照（非产品默认） |
| [`r18/swept/embdb-parent/new/`](r18/swept/embdb-parent/new/) | battle 升格前 `embdb/new` 残留 |

## 原则

- 新 peers / layout 跑完：建 `backup/<topic>/`，短 `PEERS.md` 或 `ARCHIVE.md` + 原始 `.log`
- 废止的 `docs/` 正文：进 `r18/swept/docs-hist/`（或新一轮 `swept/`），不要留在 `docs/` 分类目录
- 产品树需要的探针（如 `storm.cpp` for g9）：留在 `bench/probe/`，副本可在 swept
