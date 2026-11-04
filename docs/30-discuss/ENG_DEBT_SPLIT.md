# 工程债：拆分 `fcs.h`（机械切片）

状态：计划中 · 行为零 diff  
约束：`CONSTRAINTS.md` · skill `fcs-eng-debt`

## 目标

降低单头 ~8k 行的审阅/合并成本；**不**改变公开 API 与磁盘格式。

## 建议切面（对齐 `Mxx` TOC）

| 切片 | 内容 | 建议文件 |
|------|------|----------|
| A | M01–M02 参数与编解码 | `fcs_codec.h` |
| B | M03–M05 MemTable / Bloom / Run·Seg·Cell | `fcs_dir.h` |
| C | M06 锁 / TLS | `fcs_sync.h` |
| D | Store 公开面声明 | `fcs_store.h` |
| E | Store 实现仍可留 `fcs.h` 或按 commit/merge/mount 三分 | 后续 |

`fcs.h` 最终只 `#include` 聚合，对外仍 `#include "fcs.h"`。

## 每刀检查单

- [ ] 仅移动，无逻辑改写  
- [ ] `make bins` / `make ci-smoke` / `make test`  
- [ ] 无默认旋钮变化  
- [ ] PIT_LOG 无新增 OPEN（拆头本身不应出正确性坑）  

## 非目标

借拆头做双 memtable、P2c、API 重命名。
