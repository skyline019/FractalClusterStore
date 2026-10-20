# Soft peers（R18 产品形态）

> Soft = `set_tidy_policy(Soft)` → `configure_deploy`（async + cheap + hole_fit + 碎片 A + 护洞 + hw_reclaim）。  
> 复现：`bash backup/peers-soft/run_peers_soft.sh`  
> 全量回归 + Soft peers：`bash backup/peers-soft/run_all.sh`  
> 日期：2026-10-21（见 `META.txt`）

## make test

`MAKE_TEST_OK`（含 g9–g18、h1–h3、b1；b1 ≈ 246 s）。依赖：`bench/probe/storm.cpp`（g9 include；已从 `backup/r18/swept/bench-probe/` 恢复到产品树）。

## FCS vs SQLite（bench4，soft=1）

| 配置 | insert | update | get | range | space FCS / SQLite |
|---|---:|---:|---:|---:|---:|
| Soft sync 50k | 1.07× | 0.63× | 0.34× | 2.16× | 3.20 / 2.35 MB |
| Soft async+group+cons+shr | **0.12×** | **0.08×** | **0.20×** | **0.45×** | 2.25 / 2.35 MB |
| Soft sync 200k/100B | 1.27× | 0.55× | 0.69× | 3.51× | 37.6 / 23.5 MB（未 polish） |

（比值 = fcs/sqlite，&lt;1 表示 FCS 更快。）

## FCS vs LMDB（bench7，soft=1）

| 配置 | insert | update | get | range | space FCS / LMDB |
|---|---:|---:|---:|---:|---:|
| Soft async+cons+shr | **0.07×** | **0.06×** | 0.75× | 2.06× | 2.25 / 5.54 MB |
| Soft sync | 0.88× | 0.66× | 1.52× | 11.4× | 3.20 / 5.54 MB |

## 读法

- Soft 写路径（尤其 async+cons）相对 SQLite/LMDB 仍碾压；整理后空间贴 SQLite、优于 LMDB。
- Soft sync 未整理时 range 仍慢于 LMDB（与 EVAL17「写换读」一致）；产品侧靠 Soft 防漂移 + 对外前 `polish`。
- 单次 WSL 数字有抖动，勿抠小数；对照旧档 `backup/peers-r17/`。
