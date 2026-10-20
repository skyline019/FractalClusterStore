# WSL 真机归档 — 2026-10-20

## 环境

| 项 | 值 |
|---|---|
| 主机 | voyagerStar · WSL2 (`6.18.33.2-microsoft-standard-WSL2`) |
| CPU / 内存 | 8 核 · 11 GiB |
| 磁盘 | ext4 原生盘（`~/work/embdb`，非 `/mnt/e`） |
| 代码快照 | 与当日 `include/fcs.h`（第 14 轮 final）一致 |
| 部署路径 | `$HOME/work/embdb` ← rsync from `/mnt/e/FCS/embdb` |
| DB 路径 | `/home/user/.fcs_bench.db` · `/home/user/.sqlite_bench.db` |

## 回归

```
cd ~/work/embdb/x && make -j$(nproc) -s test
→ TEST_EXIT=0 · ~137 s（日志见 test_wsl.log，若有）
```

## bench4 vs SQLite（WAL + synchronous=FULL）

命令：

```
# sync 基线
./build/bench4 50000 500 32 50000 2000 100 0 0 0 8 0 0 0 1 0 0

# README：async + 组提交 + consolidate + shrink
./build/bench4 50000 500 32 50000 2000 100 0 1 1 8 0 1 1 1 0 0

# 放大 20 万键
./build/bench4 200000 2000 32 50000 2000 100 0 1 1 8 0 1 1 1 0 0
```

### 摘要（µs/op；比值 = FCS/SQLite，&lt;1 更快）

| 配置 | insert | update | get | range×100 | FCS 空间 | SQLite 空间 |
|---|---:|---:|---:|---:|---:|---:|
| sync 5万 | 4.32 / 0.88× | 5.20 / 0.54× | 0.53 / 0.31× | 3.99 / **1.93×慢** | 4.35 MB | 2.35 MB |
| async+整理 5万 | 0.35 / **0.07×** | 0.63 / **0.06×** | 0.31 / **0.16×** | 0.74 / **0.39×** | 2.25 MB | 2.35 MB |
| async+整理 20万 | 0.46 / 0.36× | 0.77 / 0.08× | 0.50 / 0.25× | 0.92 / 0.42× | 8.88 MB | 9.36 MB |

原始输出：`bench4_sync.log` · `bench4_async_cons.log` · `bench4_n200k.log`

## 结论

- 异步组提交 + 整理后，写入 / 覆盖 / 点查 / 区间扫描均快于本机 SQLite；空间同量级。
- 纯同步未整理时区间扫描仍慢于 SQLite（约 1.9×），与 DESIGN3 早期表一致。

## 下一轮（自 ROUND14 §8，未改代码）

1. P2 格式升版单开（`M_RUN_TRIM` / 顶层分区 / seq·ver 64 位）
2. 提交路径 fsync 出锁（先语义讨论）
3. hw 回收二步搬家（现格式可做）
