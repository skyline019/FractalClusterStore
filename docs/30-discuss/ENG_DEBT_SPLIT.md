# 工程债：拆分 `fcs.h`（机械切片）

状态：**完成**（2026-11-05）· 行为零 diff  
约束：`CONSTRAINTS.md` · skill `fcs-eng-debt` · 坑 **169**

## 目标

降低单头 ~9k 行的审阅/合并成本；**不**改变公开 API 与磁盘格式。

## 切面（对齐 `Mxx` TOC）

| 切片 | 内容 | 文件 | 行数（约） |
|------|------|------|------------|
| A | M01–M02 参数与编解码 | `include/fcs_codec.h` | 202 |
| B | M03–M05 MemTable / Bloom / Run·Seg·Cell | `include/fcs_dir.h` | 359 |
| C | M06 锁 / TLS | `include/fcs_sync.h` | 128 |
| D | M07 Store 公开面 + 实现（整块） | `include/fcs_store.h` | 8696 |
| 聚合 | M00 导读 + `#include` 链 | `include/fcs.h` | 73 |

对外仍只 `#include "fcs.h"`。依赖：`codec ← dir|sync ← store`（无环）。

脚本：`scripts/split_fcs_header.py`（可再跑；会覆盖拆出文件）。

## 持久备份（保留，勿删）

目录：`backup/eng-debt-split/`

- `fcs.h.pre-split` — 拆前整头（sha256 见同目录 `SHA256.pre-split`）
- `fcs.h.pre-split.<stamp>` / `*.post-split.<stamp>` — 本次戳记副本
- `MANIFEST.md` — 还原步骤

还原：

```sh
cp backup/eng-debt-split/fcs.h.pre-split include/fcs.h
rm -f include/fcs_codec.h include/fcs_dir.h include/fcs_sync.h include/fcs_store.h
```

## 每刀检查单

- [x] 仅移动，无逻辑改写（横幅切点机械提取）
- [x] `make bins` PASS
- [x] `make ci-smoke` PASS
- [x] `make test` PASS（`MAKE_TEST_EC=0`，log → `backup/eng-debt-split/make_test.log`）
- [x] 无默认旋钮变化
- [x] PIT_LOG **169** NOTE（拆头流程；非正确性 OPEN）

## 非目标（未做）

借拆头做双 memtable、P2c、API 重命名；Store 再按 commit/merge/mount 三分（原计划 E，仍可选后续）。
