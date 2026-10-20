# FractalClusterStore v0.1.0

首个公开里程碑：**FCS / XDB v3** 产品主树（R17 定稿 + R18 Soft 整理态），Strict CI 四门全绿。

## Highlights

- **嵌入式有序 KV**：u64 键、追加 run + 单元目录 + 分层归并 + 分片 TRIM
- **R17**：fsync 出锁、hole_fit、家族游标；写路径相对 SQLite/LMDB 优势见 `docs/00-entry/EVAL17.md`
- **R18 Soft**：护洞、`TidyPolicy::Soft` / `configure_deploy`、碎片切片 A（并洞 B 默认关）
- **工程**：`make test` / `make asan`；GitHub Actions **Strict CI**（Compile `-Werror` → Smoke → Full test → Sanitizers → **Strict CI OK**）
- **许可**：MIT

## Build & test

```sh
make          # 全量测试
make ci       # 本地对齐严格 CI（不含 sanitizers）
make asan     # ASan + UBSan + TSan
```

入口文档：[`README.md`](../../README.md) · 评估主文：[`EVAL17.md`](EVAL17.md) · 设计：[`DESIGN3.md`](../10-design/DESIGN3.md)

## Known gaps

单进程、键固定 u64；无小时 soak / 真拔电 / 多进程锁；合并峰与 LMDB range 读仍是「写换读」权衡（细节见 EVAL17）。
