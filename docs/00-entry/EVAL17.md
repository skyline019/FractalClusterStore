# 数据库内核全量评估（第 17 轮定稿 = 产品主树，滚动至 **2026-11-05**）

对象：FCS / XDB v3 — 公开 `#include "fcs.h"`（薄聚合）+ `fcs_codec/dir/sync/store.h`（坑 **169** 拆头；Store≈8.7k 行）+ `blockdev.h` / `layout.h` / `common.h`。  
**公开标签：** [v0.2.1](RELEASE_NOTES_v0.2.1.md)（拆头 + eval-full）；前序 [v0.2.0](RELEASE_NOTES_v0.2.0.md)（Soft 默认 Post）· [v0.1.0](RELEASE_NOTES_v0.1.0.md)。  
基线链：EVAL12 → R13–R15 → **ROUND16–18** → **P2 Soft-on（144）** → **Post 默认（150/151）** → **读刀 158–166 / tip 163 / 全量评 2026-11-05** → **拆头 169** → **收敛 170–172**。  
peers：历史 `peers-r17/` · `peers-soft/` · `peers-post/`；**现行 KPI** [`backup/eval-full/2026-11-05/`](../../backup/eval-full/2026-11-05/EVAL_FULL.md)。  
分叉归档：本机「小预留 TRIM 关」见 `docs/00-entry/BATTLE.md`、`backup/r18/swept/docs-hist/`（**非产品默认**）。

数字标注 **[实测]** / **[归档]** / **[推断]**。沙箱抖动常见；**peers 以 ×5 中位为准**。

---

## 0. 一页结论

| 维度 | 结论 | 最大缺口 |
|---|---|---|
| **定位** | u64 键有序 KV 嵌入式内核：追加 run + 单元目录 + 分层归并 + **分片 TRIM**；不是 B+ / 不是 SQL | 键固定 u64；单进程 |
| **正确性** | **2026-11-05**：`make test` + `soak-smoke` + `make asan` **PASS**；拆头再绿（169）；小时 soak Soft+CRC **PASS**（172，非拔电）；PIT≤172 | 无真拔电合同；无多进程锁 |
| **耐久** | 双槽 SB + 提交点 sync；页复用看 **`sb_synced_`**；`durable_vers()`；可选 `set_verify_reads` | 阶段二失败窗口变宽；`get` 默认不查数据 CRC |
| **写** | Soft async insert **~0.07–0.10×** SQL/LMDB；sync ≈SQL / **≲LMDB** | Soft sync durable = phase2 I/O 地板 |
| **读** | get ≪ SQL、≈LMDB；Post+repay 后 range ≪ SQL、对 LMDB **~1.2×** | 未 repay live Soft range vs LMDB 仍可达十几倍 |
| **空间** | sync **2.50 MB** @50k（1.12× SQL、**0.47×** LMDB）；async **2.15 MB**；Soft A1 peak_x **1.84** | P2c 未开（dead/live 门） |
| **延迟** | 提交 fsync **出锁**；读者 max 残余 **~3–4 ms** = 维护单元 | 锁内仍有 `flush_mem`；多写者不加速 |
| **寿命** | seq/ver **u64** 盘格式；惰性升 v2 | P2c 仅留 op 号 |
| **工程** | 头已拆（169）；**收敛 ACTIVE（170）**；删死枝（171）；小时 soak 已记（172） | 真掉电仍开；勿再堆热路径刀；食谱 `SOFT_RECIPE` |

**一句话**：嵌入式有序 KV——**写 + 空间 + 点读**强；产品 Soft = **Post + repay** + tip/读刀；对 LMDB 短窗 range ~**1.2×** 是非 mmap 地板（坑 167），勿默改读模型。卷宗：[`EVAL_FULL.md`](../../backup/eval-full/2026-11-05/EVAL_FULL.md)。

---

## 1. 架构画像

```
store ── cell 目录（键区间）── Seg(run, 页区间)
  run ── fences + Bloom + sp；家族 = 同层同 seq 的片
 page ── 有序记录 / 溢出链
```

- **写**：memtable → 提交 1 条 L0 run → 清单增量 → SB（阶段二锁外 fsync）。
- **读**：目录 → 段新→旧 + Bloom → fence → 页解码；`durable_vers` 可查持久进度。
- **平衡**：B1 单元字节；B2 段数；层规则 (a)(c)(d)+4t 悬崖；大组合并 → **MergeJob 分片**。
- **R16/R17 增量**：片在单元边界切开 → `M_RUN_ADD` + 每源 `M_RUN_TRIM`（锚页 page0）；下一片预留推迟到提交点后、**按洞挑片**；中等产物可 hole_fit；同 seq 片串成**虚拟源游标**。

维护 mode：0 提交不做 / 1 默认预算尾巴 / 2 调用方 `maintain` / 3 后台线程 + 读者让路。

---

## 2. 正确性与测试

| 层 | 覆盖 | 状态 |
|---|---|---|
| 回归 | g15–**g18** + 全套 | **2026-11-05** `eval-full/.../make_test.log` **PASS**；历史 peers-soft/r17 |
| 消毒 | ASan+UBSan+TSan（`setarch -R`） | **2026-11-05** `asan.log` **PASS**（~47 min，非卡死·坑 168） |
| 格式 | h2：v2 golden + v1 只读→升级→重开 | golden 须 `FCS2`（根）/ `FCSB`（`v1/`） |
| 掉电 | f3 / g6 / h3 VolDev | 绿 |
| 模糊 | g5 / h1 | 绿 |
| 账目 | g14 jobacct；g17 宽家族 >64 源 | 绿（修掩码溢出） |
| 边界 | g12 vers；g13 seq64 / v1 混链 | 绿 |

**覆盖缺口**：小时 soak；真硬件拔电；多进程；`get` 默认不 CRC；二级索引模糊权重偏低。

坑体系：DESIGN3 §12（100+）+ R16/R17 新坑（墓碑×同 seq 家族、推迟预留、hole_fit 与 g13 家族判据、fsync 阶段二短路等）——均已有测试或文档。

---

## 3. 性能

### 3.1 vs SQLite / LMDB

负载：N=50k B=500 val=32；读 50k；区间 2000×100。比值 = FCS/对端。

#### 3.1a R17 基线旋钮 [实测 `backup/peers-r17/`]

| 配置 | insert | update | get | range | space 倾向 |
|---|---:|---:|---:|---:|---|
| vs SQLite sync | 0.91 | **0.60** | **0.29** | 2.48 | 未整理偏松 |
| vs SQLite async+整理 | **0.11** | **0.07** | **0.18** | **0.44** | ≈或略优 |
| vs LMDB sync | **0.86** | **0.53** | 1.91 | 13.7 | 略紧 |
| vs LMDB async+整理 | **0.08** | **0.06** | **0.94** | 2.53 | **2.25 vs 5.54 MB** |

相对 R14 旧档：结论同向；**整理后点查 vs LMDB 从 1.25× → 0.94×**。

#### 3.1b Soft / `configure_deploy` [归档 `backup/peers-soft/`，2026-10-21]

当时产品形态（护洞 + cheap + hole_fit + 碎片 A + hw；**尚无** Post 默认）。数字保留作基线；现行见 **§3.1c**。

| 配置 | insert | update | get | range | space FCS / 对端 |
|---|---:|---:|---:|---:|---|
| vs SQLite Soft sync | 1.07 | **0.63** | **0.34** | 2.16 | 3.20 / 2.35 MB |
| vs SQLite Soft async+cons+shr | **0.12** | **0.08** | **0.20** | **0.45** | **2.25 / 2.35 MB** |
| vs LMDB Soft sync | **0.88** | **0.66** | 1.52 | 11.4 | 3.20 / 5.54 MB |
| vs LMDB Soft async+cons+shr | **0.07** | **0.06** | 0.75 | 2.06 | **2.25 / 5.54 MB** |

#### 3.1c Soft + Post 默认 [实测全量评 2026-11-05 · 坑 151+158–166]

产品 Soft：`configure_deploy` → 护洞 + P2 + **Post** + tip + 读刀；写后 **`soft_cons_repay`**。  
复现：`bash scripts/eval_full.sh` / `R=5 bash scripts/peers_full_median.sh`。  
表中比值为 **×5 轮比值中位**（推荐）；归档 peers-post（2026-11-04）仍见历史卷。

| 配置 | insert | update | get | range | space FCS |
|---|---:|---:|---:|---:|---|
| vs SQL Soft sync **Post** | **0.96** | **0.63** | **0.24** | **0.26** | **2.50 MB**（1.12× SQL） |
| vs SQL Soft async+group+cons+shr | **0.10** | **0.07** | **0.20** | **0.22** | **2.15 MB**（0.96× SQL） |
| vs LMDB Soft sync **Post** | **0.95** | **0.59** | **0.97** | **1.18** | **2.50 MB**（**0.47×** LMDB） |
| vs LMDB Soft async+group+cons+shr | **0.07** | **0.07** | **1.06** | **1.17** | **2.15 MB**（**0.41×** LMDB） |
| Soft sync live Off（未 repay）range÷LMDB [归档] | — | — | — | **~15×** | 3.05 MB |

**读法**：repay 后 range **反超 SQL**；对 LMDB 从曾 ~2.5× 压到 **~1.2×**（读刀 158–166；短窗 emit≈1 地板见坑 167）。tip KEEP；dual Negate-keep。Soft A1 peak_x **1.84**（本全量评）。

**部署整理 / 常驻整理态**：

```cpp
s.configure_deploy();          // Soft + 护洞 + P2 + SoftCons::Post（坑 144/151）
// 热路径：put…commit（Post 只记债，maybe_tidy 对 Post no-op）
s.soft_cons_repay(64);         // 读服务前还债（consolidate→relocate→checkpoint→shrink）
// 或：s.polish();             // 全库归一重锤
s.set_soft_cons(Store::SoftCons::Off);  // 消融回 live 写换读
```

| 政策 | 行为 |
|---|---|
| **Soft** | `configure_deploy`：cheap + hole_fit + 碎片 A + 护洞 + **P2** + **Post**；写中不 consolidate |
| **Hard** | Soft 旋钮 + 压力下 `consolidate_step`（Eager 语义可 `set_soft_cons(Eager)`） |
| **Periodic** | Soft + 压力下 `compact_file_step`；收口仍可 `polish` |
| **Off** | 老行为（护洞默认关；`set_tidy_policy(Off)` 亦关 soft_cons） |

养洞护航 = **allocator-only**；TRIM 不变。Eager `soft_consolidate` **勿**当 Soft 默认（坑 147）。未 repay 的 range 差距靠读前还债，勿默改合并换读。

### 3.2 提交延迟与读者 [归档 ROUND17]

| 探针 | 结果 |
|---|---|
| fsync_block（sync 睡 40 ms） | get **0.01 ms**（老头 79 ms） |
| stall 读者 | max **3.3~3.7 ms**；p99.9 **47~52 µs** |
| 残余 max | 维护单元墙钟，**不是**提交 fsync |
| 锁内 | 仍有 `flush_mem`（组提交/双 memtable 未做） |

### 3.3 其它

写放大常态约 2.2~2.8×（组提交可更低）[归档 R13/R14]。大值溢出链、共享读、多写者正确性 OK、吞吐不涨——与 EVAL14 同。

---

## 4. 空间与高水位

| 现象 | 机制 | 状态 |
|---|---|---|
| 合并峰值 | 分片 TRIM + 按洞挑片 + hole_fit | layout 200k **1.75~1.81×**（R17 det）/ 默认预算 **~1.96×**；800k **1.36~1.53×** |
| 相对 R14 | 2.60× → 1.90×（R16）→ ~1.8×（R17） | 峰值主因：「养洞 + 中等产物」；hole_fit 已削一刀 |
| 护洞（R18） | L0 不吃中部大洞 | **200k 无 KPI 收益**；800k **−0.04×**；Soft 默认开 |
| 碎片切片 A（R18） | `choose_part` 碎片时降 floor | det200k **1.82→1.71**；runs↑；零写放大；默认开 |
| 有界并洞 B（R18） | 预留前搬挡路小 run | **layout 否定**（1.71→1.74）；默认关；`set_part_coalesce` 可选 |
| 开始前脏源 | 合并未开始已 1.3~1.5× | 层规则/时机旋钮 A/B **否定**（默认不变） |
| 静态高水位 | `set_hw_reclaim_pct` + **二步搬家**（g15） | 可选；治合并后占用 |
| 还给 OS | shrink / compact_file / discard / `polish` | 默认不自动截 |
| **旁路 L** | 小预留 TRIM 关 → 峰 1.03× | 协议未闭环，**不当 KPI** |

---

## 5. 边界与寿命

| 量 | 上限 / 状态 |
|---|---|
| 键 | u64（高位索引/范围墓碑） |
| 值 | ≤65534 B；大值溢出链 |
| 页号 | u32 × 4 KB（b1 >256 MB 复验） |
| seq / ver | **盘上 u64**（FCS2）；惰性升级 |
| 快照 | ≤8；在飞提交先 `drain_inflight` |
| 读者槽 | 32 共享，溢出独占 |
| 格式 | `mounted_format` 1/2；`fmt>2` → UNSUPPORTED |

---

## 6. 风险清单（按必撞 / 静默）

| # | 问题 | 严重度 | 状态 |
|---|---|---|---|
| 1 | 合并峰仍 ~1.7×（养洞） | 中高（小库余量） | R18 A：**1.82→1.71**；B 否定；下一刀 P2c 空间 / 稀疏产物 |
| 2 | 未 repay 的 Soft live range vs LMDB | 中 | **`soft_cons_repay` / `polish`**（§3.1c）；单 run 地板 ~2.5× |
| 3 | 锁内 `flush_mem` | 中（延迟） | fsync 已出锁；组提交未做 |
| 4 | 阶段二失败窗口 ms 级 | 低~中 | 文档化；粘性 `cfail_` |
| 5 | `get` 不查 CRC | 低~中 | 建议 `set_verify_reads` |
| 6 | 多写者不加速 | 低 | 设计 |
| 7 | 无 soak / 真掉电 / 多进程 | 覆盖 | 流程债 |
| 8 | 单头 ~8.6k 行 | 工程 | **已拆**（坑 169 · ENG_DEBT_SPLIT 完成） |
| — | 合并峰 ≈2×「格式不可避」 | ~~高~~ | **R16/R17 已打破叙事** |
| — | 提交 fsync 挡读者 | ~~高~~ | **R17 已解** |
| — | 作业 TRIM 丢键（坑 110） | ~~高~~ | **R16 挂链+TRIM 闭环** |
| — | 源 >64 live 漂移 | ~~高~~ | **R17 已修** |

---

## 7. 与对等物定位

| | FCS (R17) | LMDB | SQLite (WAL FULL) |
|---|---|---|---|
| 模型 | 追加 + 分片 TRIM | mmap B+ | B+ + SQL |
| 写 | **极强**（async+整理） | 中 | 覆盖写弱于 FCS |
| 点查 | 强（整理后 ≈ LMDB） | **最强/并列** | 中 |
| 区间 | 依赖整理 | **最强** | 强 |
| 空间 | 整理后紧；峰 ~1.8× | 偏松 | 中 |
| 嵌入 | 单进程、可调维护 | 多进程友好 | 通用 |

LittleFS：**暂缓**（非有序 KV）。

---

## 8. 下一轮优先级

1. **P2c / 稀疏产物**（可选）：再压峰勿指望并洞 B / cliff。  
2. **组提交 / 双 memtable**（DISCUSS15）——卸锁内 `flush_mem`（须消融）。  
3. 工程：长 soak、真掉电矩阵；`fcs.h` 拆头已完成（坑 169）；默认仍关的 `verify_reads`。  
4. **不要**：为追 LMDB range 默改读模型；勿把 Eager 升 Soft 默认。

---

## 9. 复现入口

```sh
cd embdb/x
bash scripts/eval_full.sh          # test+soak+asan+peers×5+soft-a1 → backup/eval-full/<date>/
# 或分项：
make test && make soak-smoke && make asan
R=5 bash scripts/peers_full_median.sh
make soft-a1
# 进度可见的 asan：bash scripts/asan_progress.sh
# 历史：make peers-post · soft-post-on · peers-soft/ · peers-r17/
```

历史：`backup/r18/swept/docs-hist/`；本文件是 **R16–R18 + Post + 读刀/tip + 2026-11-05 全量评** 的滚动结论。
