# Battle 对比：两包 × 本机分叉 R16

> **2026-10-20：包 B 已升为产品主树 `embdb/x`**（`fcs.h` 7851 行）。L 快照在 `backup/pre-battle-promote/`；评估滚动见 `docs/00-entry/EVAL17.md`。
>
> **升格注意**：
> 1. zip 内文件 mtime 多为 1980，`rsync` 后务必 `touch` 源并删旧 `build/*` 测试二进制，否则混跑 L 时代 g15/b1。脚本：`backup/r17/rebuild_test.sh`。
> 2. `tests/golden/*.img` 必须为 **`FCS2`**（v1 在 `golden/v1/`）。若根目录镜像误成 `FCSB`，从 `backup/r18/swept/embdb-parent/new/x/tests/golden/` 再 rsync 一次。

来源（原 `embdb/new/` zip，现归档 `backup/r18/swept/embdb-parent/new/`）：

| 代号 | zip | 时间 | 内容 |
|---|---|---|---|
| **A** | `workspace-01a15904-…cfb4.zip` | 20:10 | ROUND16 定稿（P2a+P2b），`fcs.h` 7626 行 |
| **B** | `workspace-01a1593c-…d1a4.zip` | 22:05 | A + ROUND17（fsync / hole_fit / 家族游标），`fcs.h` 7851 行 |
| **L** | `embdb/x`（本会话） | — | 另一条 R16：每片小预留、**不开**作业 TRIM；layout 峰值 1.03× |

**合并结论**：以 **B 为唯一主干**（已升格为 `embdb/x/`）。A ⊂ B；L 与 B 盘格式/作业协议不兼容，只作对照，不混编。

---

## 1. 三方对照（合并水位 + 正确性）

| 维度 | L（本机小预留） | A（P2b 分片 TRIM） | **B（A+R17）** |
|---|---|---|---|
| 格式 | 仍 R15 系（本机已有 fmt=2 半截） | **FCS2** + MENT48 + seq/ver u64 | 同 A |
| 作业 TRIM | 关（坑 110/113） | **开**：片封口 + `M_RUN_TRIM`，锚页留 page0 | 同 A + 中等产物 hole_fit |
| 预留策略 | 每片 ~`merge_budget`，writer 跨片续写 | `choose_part` 按洞挑片；封口后**推迟**到下一提交点再预留 | 同 A + `<1024` 页也可按洞切 |
| layout 200k peak | **1.03×** | **1.90×** | **1.75~1.81×** |
| layout 800k peak | （未系统复测） | **1.40×** | **1.36~1.37×** |
| 读者 vs 慢 fsync | 锁内双 sync（旧） | 同旧 | **出锁**：79 ms → 0.01 ms（睡盘探针） |
| >64 源归并 | 掩码风险仍在 | 同（默认负载源≤32 未爆） | **修**：家族链游标；famw CORRUPT→OK |
| 回归 | g8/g13/g14/g15 + make test | g16 + 47 段 | **g17 + 48 段**，asan 绿 |
| 能否当产品默认 | 峰值好看，TRIM 路径未通 | 可 | **应取** |

解读：

- L 的 1.03× 来自「不去叠整组流式预留」，**源页仍占到收尾**；和 A/B「边合边 TRIM 源前缀」不是同一问题。
- A 把 TRIM 做对了，但峰值卡在「养洞 + 中等产物落尾」。
- B 否掉了「改时机旋钮」的幻想，用 **hole_fit** 再压一档，并卸掉 fsync 挡读者 + 修宽家族账漂移。

---

## 2. A → B 增量（值得保留的优化）

1. **① fsync 出锁**（`commit_phase1/2/3` + `durable_vers`）— 延迟硬顶，默认开。
2. **② 时机旋钮保留、默认不变** — A/B 实测 big30/cliff2 更差；文档作废 R16「脏 L2 等悬崖」归因。
3. **②(c3) `part_hole_fit`** — 256~1023 页产物按最大洞切 2~4 片；200k **~2.0×→~1.8×**，写量不变。
4. **③ `build_chains` 虚拟源** — 正确性必修（>64 源）；为更小 floor / P2c 铺路。

A 独有且已进 B：P2a 惰性升级、按洞 `choose_part`、推迟预留、`older_may_contain_grp`、g16 六段。

---

## 3. L 分支还剩什么价值

| 可借鉴 | 不要混进 B |
|---|---|
| 「开作业勿整组 `begin_stream`」的峰值意识（B 已用 choose_part / 推迟预留表达） | 片间不 seal + TRIM 关掉的路径 |
| g14 `t_reserve`「峰值 ≪ 源页老估」判据思路 | 本机 ROUND16.md 叙事（与包内 ROUND16 同名异义） |
| EVAL14 对等物定位 | 把 L 的 `fcs.h` 盖到 B 上 |

若要把 L 的极致峰值并进 B，只能走 **P2c 稀疏 run / 养洞侧合并**（ROUND17 §2.5），不能回退 TRIM。  
ROUND18 已落地「分配器不吃大洞」：机制正确、**200k layout 峰无益**、800k −0.04×（`docs/20-rounds/ROUND18.md`）——不够替代 L 的 1.03× 叙事。

---

## 4. 产品主树

- **`embdb/x` = 包 B（R17）+ R18 整理态**；全量评估见 `docs/00-entry/EVAL17.md`。
- peers：R17 基线 `backup/peers-r17/`；**Soft 产品形态** `backup/peers-soft/`（2026-10-21）。
- 升格残留树见 `backup/r18/swept/embdb-parent/new/`；zip 在 `E:/FCS/backup/`。
