# 对照基线：FCS (R17) / SQLite / LMDB（2026-10-20 重跑）

主机：WSL2 `voyagerStar`（见 `META.txt`）；`fcs.h` 7851 行、`FCS_R17=1`。  
复现：`bash backup/peers-r17/run_peers_r17.sh` → 本目录日志。  
旧档对照：`backup/wsl-20261020/`（R14 时代同参）。

**LittleFS**：仍暂缓（非有序 KV）。

单位 µs/op（越小越好）；比值 = FCS / 对端（&lt;1 表示 FCS 更快）。

---

## 1. FCS vs SQLite（bench4，N=50k B=500 val=32）

### sync（默认同步提交）

| phase | FCS | SQLite | FCS/SQLite | 旧档 (wsl-20261020) |
|---|---:|---:|---:|---:|
| insert | 4.56 | 5.03 | **0.91** | 0.88 |
| update | 5.68 | 9.48 | **0.60** | 0.54 |
| get | 0.51 | 1.80 | **0.29** | 0.31 |
| range | 4.67 | 1.88 | 2.48 | 1.93 |
| space | 4.35 MB | 2.35 MB | 更松 | 同量级 |

### async + 组提交 + consolidate + shrink

| phase | FCS | SQLite | FCS/SQLite | 旧档 |
|---|---:|---:|---:|---:|
| insert | 0.50 | 4.55 | **0.11** | 0.07 |
| update | 0.78 | 11.52 | **0.07** | 0.06 |
| get | 0.39 | 2.13 | **0.18** | 0.16 |
| range | 0.86 | 1.97 | **0.44** | 0.39 |
| space | 2.25 MB | 2.35 MB | **略省** | 同 |

要点：整理后写/点查仍碾压 SQLite；range 整理后可快于 SQLite。sync 时 range 仍慢 ~2.5×（未整理 run 多）。绝对 µs 有抖动，比值与旧档同结论。

### 4× 规模 sync（N=200k B=2000 val=100）

| phase | FCS | SQLite | FCS/SQLite |
|---|---:|---:|---:|
| insert | 1.85 | 1.44 | 1.28 |
| update | 6.03 | 12.67 | **0.48** |
| get | 1.42 | 2.48 | **0.57** |
| range | 10.89 | 2.85 | 3.82 |
| space | 35.8 MB | 22.4 MB | 更松（未整理） |

---

## 2. FCS vs LMDB（bench7，N=50k B=500 val=32）

### async + 组提交 + consolidate + shrink

| phase | FCS | LMDB | FCS/LMDB | 旧档 |
|---|---:|---:|---:|---:|
| insert | 0.39 | 4.63 | **0.08** | 0.05 |
| update | 0.63 | 10.90 | **0.06** | 0.06 |
| get | 0.30 | 0.32 | **0.94** | 1.25 |
| range | 0.88 | 0.35 | 2.53 | 2.06 |
| space | 2.25 MB | 5.54 MB | **更省** | 同 |

### sync 基线

| phase | FCS | LMDB | FCS/LMDB | 旧档 |
|---|---:|---:|---:|---:|
| insert | 4.38 | 5.10 | **0.86** | 1.10 |
| update | 5.57 | 10.46 | **0.53** | 0.53 |
| get | 0.59 | 0.31 | 1.91 | 1.69 |
| range | 4.60 | 0.34 | 13.7 | 11.6 |
| space | 4.35 MB | 5.54 MB | 略省 | 同 |

要点：整理后写仍远快于 LMDB；**点查已追平/略胜**（旧档 1.25× → 现 0.94×）；range 仍是 mmap B+ 更强（~2.5× / sync ~14×）。空间 FCS 更紧。

---

## 3. 相对旧档一句话

R17 主树 peers **结论不变**：写优先配置下 FCS 碾压 SQLite/LMDB；读路径 LMDB range 仍最强，点查差距收窄。绝对数字有沙箱抖动，勿抠小数；空间与旧档一致（整理后 ~2.25 MB @50k）。
