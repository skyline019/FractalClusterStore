# 工程债：Soak + 读路径 CRC

## CRC（坑 120）

| 项 | 决定 |
|----|------|
| API | `set_verify_reads(bool)` / `verify_reads()` |
| 默认 | **false**（与 DESIGN3：读路径每页 CRC 成本高） |
| 校验点 | `page()` 从设备装填缓存（及 TLS 私有缓冲）之后 |
| 范围 | `PK_DATA` 且 `used` 合法；与 `verify`/`scrub` 同式 |
| 失败 | 槽位保持/恢复为未缓存；返回 `nullptr` |
| 测试 | `tests/c2.cpp` |

## Soak（坑 121）

| 项 | 决定 |
|----|------|
| 二进制 | `tests/soak1.cpp` |
| 短门 | `make soak-smoke` → `FCS_SOAK_SEC=30`（可调） |
| 长跑 | `FCS_SOAK_SEC=3600 make soak`（人工/夜间） |
| 内容 | 写混合 + 每 N 次 `verify` + 偶发 `scrub` + reopen |
| **不是** | 真拔电矩阵（仍靠 f3/g6/h3） |

## 完成判据

- `c2` 进入 `make test`  
- `soak-smoke` 在 Makefile 可选目标；CONSTRAINTS 已写明  
- PIT_LOG 120 FIXED；121 在短门稳定后可改为 FIXED（长时仍建议周期性跑）  
