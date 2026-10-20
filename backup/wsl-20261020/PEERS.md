# 对照计划：FCS / LMDB /（LittleFS 暂缓）

> **更新**：R17 主树重跑结果见 [`../peers-r17/PEERS.md`](../peers-r17/PEERS.md)。下文为升格前 WSL 归档，保留对照。

## 现状（2026-10-20）

**暂时不对比 LittleFS。**

| 引擎 | 状态 | 原因 |
|---|---|---|
| FCS | ✅ 对照中 | 有序 KV |
| LMDB | ✅ 对照中 | 有序 KV，公平同级 |
| LittleFS | ⏸ 暂缓 | 闪存文件系统，非有序 KV；文件-per-key 在 5 万规模极慢；禁测 range |

源码仍保留：`littlefs-master.zip` → `x/bench/third_party/littlefs/`（需要时 `-DBENCH7_WITH_LFS` 可再开，仍禁测 LFS.range）。

## bench7 现行（仅 FCS↔LMDB）

```
./build/bench7 N B VSZ R S K async gm cons shr
# 例：async+整理
./build/bench7 50000 500 32 50000 2000 100 1 1 1 1
```

指标：insert / update / get / range / space；每 B 次一持久屏障；LMDB 大端键。

## WSL 真机结果（2026-10-20，日志 `bench7_*.log`）

负载：N=50000 B=500 val=32B R=50000 S=2000×100。单位 µs/op；比值 = FCS/LMDB（&lt;1 表示 FCS 更快）。

### async + 组提交 + consolidate/shrink

| phase | FCS | LMDB | FCS/LMDB |
|---|---:|---:|---:|
| insert | 0.35 | 6.84 | **0.05** |
| update | 0.66 | 11.85 | **0.06** |
| get | 0.40 | 0.32 | 1.25 |
| range | 0.73 | 0.36 | 2.06 |
| space | 2.25 MB | 5.54 MB | 更省 |

### sync 基线（无 async/组提交/整理）

| phase | FCS | LMDB | FCS/LMDB |
|---|---:|---:|---:|
| insert | 6.12 | 5.56 | 1.10 |
| update | 5.07 | 9.58 | **0.53** |
| get | 0.55 | 0.33 | 1.69 |
| range | 4.03 | 0.35 | 11.6 |
| space | 4.35 MB | 5.54 MB | 略省 |

要点：整理后的 FCS 写路径远快于 LMDB；点查/区间 LMDB 仍更强（mmap B+）；空间 FCS 更紧。
