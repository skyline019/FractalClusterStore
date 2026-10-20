# 数据库内核全量评估（第 17 轮定稿 = 产品主树，滚动至 2026-10-21）

对象：`include/fcs.h`（FCS / XDB v3，**~8118 行**，`FCS_R17`）+ `blockdev.h` / `layout.h` / `common.h`。  
基线链：EVAL12 → R13–R15 → **ROUND16（P2a+P2b）** → **ROUND17（fsync 出锁 / hole_fit / 家族游标）** → **ROUND18（护洞 + 整理状态机）**。  
peers：`backup/peers-r17/`（R17 基线）；**Soft 产品形态** `backup/peers-soft/`（2026-10-21）；旧档 `backup/wsl-20261020/`。  
分叉归档：本机「小预留 TRIM 关」见 `docs/00-entry/BATTLE.md`、`backup/r18/swept/docs-hist/ROUND16_L_alt.md`、`backup/pre-battle-promote/`（**非产品默认**）。

数字标注 **[实测]** / **[归档]** / **[推断]**。沙箱 2 核抖动常见；peers 单次 WSL。

---

## 0. 一页结论

| 维度 | 结论 | 最大缺口 |
|---|---|---|
| **定位** | u64 键有序 KV 嵌入式内核：追加 run + 单元目录 + 分层归并 + **分片 TRIM**；不是 B+ / 不是 SQL | 键固定 u64；单进程 |
| **正确性** | 全量 `make test` 绿（含 g15–**g18**、h1–h3、b1；2026-10-21 Soft 轮）；P2 格式/TRIM/墓碑/宽家族/护洞整理态 | 无小时 soak；无真拔电；无多进程锁 |
| **耐久** | 双槽 SB + 提交点 sync；页复用看 **`sb_synced_`**；`durable_vers()` | 阶段二失败窗口变宽（ms 级）；`get` 默认不查数据 CRC |
| **写** | async+组提交+整理：≪ SQLite / ≪ LMDB（insert **0.08~0.11×**） | 默认同步时写接近/略慢 SQLite insert（200k） |
| **读** | 点查强于 SQLite、**追平 LMDB**（整理后 0.94×）；range 整理后可快于 SQLite、仍慢于 LMDB **~2.5×** | 未整理 range vs LMDB **~14×** |
| **空间** | 整理后 ≤ SQLite、远紧于 LMDB（2.25 vs 5.54 MB @50k）；合并峰 **~1.8×** 活（200k） | 养洞（N 小 TRIM 洞）；护洞对 200k 峰无 KPI 级收益（ROUND18）；P2c 未做 |
| **延迟** | 提交 fsync **出锁**（慢盘探针 79→0.01 ms）；读者 max 残余 **~3–4 ms** = 维护单元 | 锁内仍有 `flush_mem`；多写者不加速 |
| **寿命** | seq/ver **u64** 盘格式；惰性升 v2；重编号/FULL 闸保留 | P2c 稀疏 run 仅留 op 号 |
| **工程** | 文档/测试密；旋钮多；单头 7.8k 行 | 拆头 / 本地 git / soak |

**一句话**：作为嵌入式有序 KV，**正确性 + 写路径 + 提交不挡读者**是强项；合并峰 det200k **~1.71×**（TRIM+hole_fit+碎片 A）；并洞 B 已否定；Soft 整理态已进产品入口；相对 LMDB 仍是「写换读」；再压峰看稀疏产物，P2c 走空间轨。

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
| 回归 | g15–**g18**（护洞/整理态/碎片 A/并洞 B）+ 全套 | `backup/peers-soft/make_test.log` **MAKE_TEST_OK**（2026-10-21）；历史 `backup/r17/test2.log` |
| 消毒 | ASan（含 g16/g17）；TSan g17 多写者 | `backup/r17/asan.log` |
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

#### 3.1b Soft / `configure_deploy` [实测 `backup/peers-soft/`，2026-10-21]

产品常驻形态（`soft=1`：护洞 + cheap + hole_fit + 碎片 A + hw）。复现：`bash backup/peers-soft/run_peers_soft.sh`。

| 配置 | insert | update | get | range | space FCS / 对端 |
|---|---:|---:|---:|---:|---|
| vs SQLite Soft sync | 1.07 | **0.63** | **0.34** | 2.16 | 3.20 / 2.35 MB |
| vs SQLite Soft async+cons+shr | **0.12** | **0.08** | **0.20** | **0.45** | **2.25 / 2.35 MB** |
| vs LMDB Soft sync | **0.88** | **0.66** | 1.52 | 11.4 | 3.20 / 5.54 MB |
| vs LMDB Soft async+cons+shr | **0.07** | **0.06** | 0.75 | 2.06 | **2.25 / 5.54 MB** |
| vs SQLite Soft sync 200k/100B | 1.27 | **0.55** | **0.69** | 3.51 | 37.6 / 23.5 MB（未 polish） |

**读法**：Soft 写路径与 R17「手拧 async+整理」同量级；整理后空间贴 SQLite、远紧于 LMDB。Soft sync 点查仍强于 SQLite；未整理 range vs LMDB 仍大（sync ~11×）——靠 Soft 防漂移 + 对外前 `polish`，与 §0 缺口一致。本轮 Soft async 点查 vs LMDB **0.75×**（略优于 R17 档的 0.94×，单次抖动勿抠）。

**部署整理 / 常驻整理态**：

```cpp
s.set_tidy_policy(Store::TidyPolicy::Soft);  // 默认产品形态：旋钮 + 护洞
s.configure_deploy();                        // async + cheap + hole_fit + 护洞 + hw 150
// 热路径：commit(false)…commit()
while (s.maybe_tidy() == RC::MORE) {}        // Hard / Periodic 才迈步；Soft 为 no-op
s.polish();                                  // 对外读服务前重锤
```

| 政策 | 行为 |
|---|---|
| **Soft** | `configure_deploy`：cheap + hole_fit + **碎片切片 A** + 护洞；不主动归一 |
| **Hard** | Soft + 压力下 `consolidate_step`（`set_tidy_runs` / layout_pct） |
| **Periodic** | Soft + 压力下 `compact_file_step`；要贴整理列再 `polish` |
| **Off** | 老行为（护洞默认关） |

养洞护航 = **allocator-only**（非 `M_RUN_HOLE`）；TRIM 协议不变。layout A/B 见 `docs/20-rounds/ROUND18.md`：200k 峰 **1.96→1.97**（无益）/ 确定性 **1.82→1.81**；800k **1.53→1.49**；写量不变。未整理 range 差距靠 Soft 防漂移 + 空闲 `polish`。

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
| 2 | 未整理 range vs LMDB | 中 | **Tidy Soft + `polish`**（见 §3） |
| 3 | 锁内 `flush_mem` | 中（延迟） | fsync 已出锁；组提交未做 |
| 4 | 阶段二失败窗口 ms 级 | 低~中 | 文档化；粘性 `cfail_` |
| 5 | `get` 不查 CRC | 低~中 | 建议 `set_verify_reads` |
| 6 | 多写者不加速 | 低 | 设计 |
| 7 | 无 soak / 真掉电 / 多进程 | 覆盖 | 流程债 |
| 8 | 单头 7.8k 行 | 工程 | 可拆 |
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

1. **P2c / 稀疏产物**：A 已吃 −0.11×；B 已否定（ROUND18 §6）。再压峰勿指望并洞。P2c 走压实空间。勿再拧 cliff/护洞门槛。  
2. **组提交 / 双 memtable**（DISCUSS15 未做）——卸锁内 `flush_mem`。  
3. 工程：soak、真掉电、拆头、本地 git、`set_verify_reads`。

---

## 9. 复现入口

```sh
cd embdb/x
make test                           # 含 g18；需 bench/probe/storm.cpp（g9）
./build/g16 && ./build/g17 && ./build/g18 && ./build/h2
# 护洞 A/B：backup/r18/run_layout_{ab,det}.sh → docs/20-rounds/ROUND18.md
bash backup/peers-r17/run_peers_r17.sh     # R17 基线旋钮 → backup/peers-r17/
bash backup/peers-soft/run_peers_soft.sh   # Soft 产品形态 → backup/peers-soft/
# 或：bash backup/peers-soft/run_all.sh    # make test + Soft peers
# 文档：ROUND16–18.md BATTLE.md DESIGN3 §13.15–13.16
```

历史：`backup/r18/swept/docs-hist/`（EVAL12 / EVAL14_L_alt / DISCUSS10–12…）；本文件是 **R16+R17+R18+Soft peers** 的滚动全量结论。
