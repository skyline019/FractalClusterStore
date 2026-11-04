# FCS 坑日志（活文档）

> **纪律**：新正确性/静默失败缺陷 → 本表**先占号**再合修复；`FIXED` 必须带回归测 id。  
> **收工**：任务结束若有坑、近误判、环境陷阱或注意点 → **同轮更新本表**（勿只留在对话里）。  
> **根因**：单一线索不够；须复现 + 排除竞品假说 + 独立复核后再写「verified」。未证前写 leading。  
> 叙事归档仍在 [`DESIGN3.md`](DESIGN3.md) §12；讲解页 `fcs-pits.html`。  
> 技能：`.cursor/skills/fcs-pit-discipline`。

## 状态

| 状态 | 含义 |
|------|------|
| OPEN | 已复现或已定性，修复未合入或回归未齐 |
| FIXED | 已合入 + 有回归测/探针（或明示 WONTFIX 例外） |
| WONTFIX | 书面接受；须写清理由与缓解 |
| NOTE | 环境/流程注意（非产品正确性缺陷，但仍要登记以免再踩） |

## 编号

- 历史坑 1–112+ 见 DESIGN3 §12 / `fcs-pits`（本表从**工程债轨起**补登记缺口与新号）。
- 下一可用号：**139**。

## 工程债轨登记

| ID | 状态 | 症状 | 不变量 / 修法（须标 leading / verified） | 回归 / 探针 |
|----|------|------|------------------------------------------|-------------|
| 113 | FIXED | 作业 TRIM 丢键（R16） | verified：挂链+TRIM 闭环 | `g16` · EVAL17 |
| 114–119 | — | R17/R18 扩展位（fsync 阶段二 / 家族 / 护洞…） | 见 ROUND17–18；补录时改状态 | `g17`/`g18` |
| **120** | FIXED | `get`/读路径默认不校验数据页 CRC，介质位翻转可静默吐坏键 | verified：`set_verify_reads(true)` 时 `page`/`prefetch` 装填校验；开启清缓存；失败不入缓存。**近误判**：曾翻「已退役 PK_DATA 残骸」或 `used` 外字节 → get/scrub 仍绿；活页+`[12,used)` 内翻位才成立 | `c2` · `fcs.h` `set_verify_reads` / `data_page_crc_ok` |
| **121** | FIXED | 无门禁化短时 soak（长时仍人工） | verified：`soak1` 混合写+verify+scrub+reopen；`make soak-smoke` | `soak1` · Makefile |
| **122** | NOTE | `make asan` 在 TSan 步 FATAL `unexpected memory mapping`（WSL 6.18） | **leading 曾疑**：产品数据竞争 / 地址耗尽。**verified**：同二进制反复 FATAL；`setarch $(uname -m) -R` 稳定 PASS；ASan 段已全绿。属 ASLR/TSan 运行时与新内核不兼容，非引擎竞态。Makefile `TSAN_WRAP`；`scripts/tsan_env_probe.sh` | `scripts/asan_tsan_tail.sh` → `ASAN_TSAN_TAIL_OK` |
| **123** | NOTE | 工程债一期收口 | verified：CRC+短 soak+严格门+坑纪律达标；**拆头未做**；不可宣称 EVAL17 工程项全部关闭。全文 `docs/00-entry/EVAL_ENG_DEBT_P1.md` | 见该评估 §2 门禁表 |
| **124** | NOTE | 空间轨探讨拍板 | verified 决议：R→T→可选P2c；A2=256/512/1024；卸载第一刀=护洞+近满加速；测=轻量+asan-new，全量后期。全文 `DISCUSS_HARNESS_AB_SPACE.md` | `make harness-ab-space` |
| **125** | FIXED | Soft 近满时维护仍守预算，回收偏晚（空间峰/可写性） | verified 机制：`space_accel` 近满视同 backlog + NOSPACE 前 `hw_reclaim`。**消融**（`harness-ab-space` 2026-11-04）：A1/A2×3/A3 峰与消融臂无差 ⇒ **Soft 默认关**；旋钮保留。leading：A2 峰顶在 `user_lim`；A1/A3 不近满。见 `backup/harness-ab-space/VERDICT.md` | `c3` · `make harness-ab-space` |
| **126** | NOTE | Soft A1 峰值归因落地 | **verified Soft A1**（`make soft-a1`）：peak **3428 / 1.860×**（同 harness）；插入期 6×(+311) `maxh<64`+`pend=328` ⇒ **HOLE_STARVE**；峰提交 `r0=1.333` `pend=987` L3 波；recount 后 **dead/live=0.023 ⇒ 勿开 P2c**。见 `backup/soft-a1/VERDICT.md` · `bench/probe/soft_a1.cpp` | `make soft-a1` |
| **127** | NOTE | 页级 alive/dead 存货 | **verified**（Soft A1 峰 `LIVE_INV`）：无 per-page live bit；峰时 **DATA_DEAD=0**、DATA_MIX=1152、DATA_LIVE=605；**PEND=987+FREE=536 ≈44%·tail**；记录 dead_frac=0.029。峰=隔离占用+养洞，非全死页。全文 `backup/soft-a1/ALIVE_DEAD.md` | `soft_a1 … 1` dump_live_inv |
| **128** | NOTE | 死碎片分布与机制 | **verified Soft A1 FRAG**：点覆盖⇒页内三分位≈均、streak~1–3；合前 L2 dead_frac=0.25 且 dense MIX 多，峰时并入 PEND、残死 sparse(3%)。机制=flush `probe_prev` 记账不改页；回收=整 run/TRIM/合并非抠洞。`backup/soft-a1/FRAG_DEAD.md` | `FRAG dens_hist/tertiles` |
| **129** | NOTE | MIX 活链/动态/虚拟化探讨 | leading 策略序：峰→养洞/PEND；稀疏 MIX 忽略；dense→可选空闲整 run 重写(B2)；P2c 仅对 DATA_DEAD；拒持久键转发。无损=热读/热写零改+默认关消融。全文 `docs/30-discuss/DISCUSS_MIX_LIVE_VIRTUAL.md` | 探讨未实施 |
| **130** | NOTE | 限制下彻底解的空间模型 | verified 框架：彻底=\(\mathcal{I}\) 上逼近下界；峰=\(L_{pre}+\Delta_{prod}+P-F_{reuse}\)；开放主自由度=洞片匹配+P错开；MIX盐粒与最小P为合法剩余。全文 `docs/30-discuss/DISCUSS_SPACE_MODEL.md` | 建模；下一刀 P1 分配器 |
| **131** | NOTE | 跨学科概念→组合建模 | leading 合成：1D晶格气体+空位渗流+RG合并；峰=ξ&lt;part堵塞+INT(PEND)过饱和；MIX=替位无序。检索锚：Coffman碎片/LSM粒度/持续同调/free-volume。全文 `docs/30-discuss/DISCUSS_SPACE_MATHPHYS.md` | 探讨；观测ξ,β0(part) |
| **132** | NOTE | P1 流式整洞跳过 Soft A1 抬峰 | verified：薄谓词 Soft A1 on peak_x **4.15** vs off **1.80**。**勿升 Soft 默认**。见 `backup/soft-a1/VERDICT_P1.md` | `make soft-a1-p1`（旧语义） |
| **133** | NOTE | P1b 流式 anti-shatter Soft A1 仍抬峰 | verified：`c4` PASS；Soft A1 on peak_x **2.20** vs off **1.86**（薄 P1 曾 4.15）。**勿升 Soft 默认**。见 `backup/soft-a1/VERDICT_P1b.md` | `c4` · `make soft-a1-p1` |
| **134** | NOTE | P1c best-fit 降峰但 A2/写延迟未过门 | verified 峰 SoftA1/A1↓；A2 未同向；Soft sync×5：insert med **+11.8%**。**勿升 Soft**。`VERDICT_P1c.md` | `c5` · soft-a1-bf · soft-a1-bf-lat |
| **135** | NOTE | P1d bf 消融最优=small+skip180 | verified：峰降≈15% 且 insert 中位无税（−3%）；full 峰更好但 insert+28%。A2 仍微负 ⇒ **Soft 勿升**。推荐实验 `mode=2,skip=180`。`VERDICT_P1d.md` | `c5` · `make soft-a1-bf-ablate` |
| **136** | NOTE | frag 时空探针 | Soft 逐提交 timeline + 事件点 holes 全量转储（FREE/PEND/DEF）。`make frag-trace` → `backup/frag-trace/` | `bench/probe/frag_trace.cpp` |
| **137** | NOTE | Soft A1 frag 分布（n200k） | verified：peak 3354@c547；STARVE×12≈每64c；starve 时 PEND **8×41=328**、FREE maxh&lt;64；峰 FREE 21seg/704pg 全&lt;64（众数51）。β PEND相位+α 出生共因；γ=P1d 缓解。`VERDICT_FRAG.md`。探针曾用 PEAK 覆盖同提交 STARVE 标签（已分打） | `make frag-trace` · analyze_frag_* |
| **138** | NOTE | P2 洞饿根源刀（starve_fit/promote） | verified Soft A1：both 峰↓≈8%；fit 单开害、prom 单空。门禁 A2/A3 空间过、update+16% ⇒ Soft 勿升。**peers**（`PEERS_P2.md`）：vs SQL/LMDB 叙事不变；sync both/off 写+6~8%；async 碾压同量级；空间不变 | `c6` · soft-a1-p2 · soft-a1-p2-gate · peers-p2 |
| **139** | NOTE | P2 Soft A1 写税源与税刀 | verified：**源**=α 片过碎（floor16→多片）放大 β 每片 `commit_delta`（prom≈13）；50k Soft sync 上 P2 计数常 0（休眠噪声）。**刀**=`STARVE_FLOOR=32`+`MAX_SLICES=12`+`MAX_PROMOTES=1`。×3 中位 both 峰 1.907→1.744 (−8.5%)、insert +2.7%（刀前单次≈+39%）。β=4/作业峰近平（弃）。Soft 仍关 | `c6` · `scripts/a1_p2_tax_median.sh` · `VERDICT_P2_TAX_A1.md` |
| **140** | NOTE | P2 税刀矩阵+抗抖确认 | verified：配对 Δ×5（warmup 弃、`setarch -R`）。矩阵 PASS 胜者 **`PROM_MIN_PEND=64`**：med Δpeak **−14.7%**（IQR 3.1）· Δins +8.8%。确认轮 peak **−14.2%** vs cur −9.9%；insert Δ IQR 大（勿报加速）。默认写入宏；Soft 仍关 | `a1_p2_tax_matrix.sh` · `a1_p2_tax_confirm.sh` · `VERDICT_P2_TAX_MATRIX.md` |
| **141** | FIXED | P2 β 持锁 `write_sb` 写税根 | verified：**根**=`merge_job_step`→`commit_delta`→`write_sb` 在 `maint_unit` **XLock 内** drain+双 sync ⇒ put 堵。**刀**=defer：`starve_prom_flush_` 锁内 fold+capture、锁外 `sb_flush`、再 promote。配对×5：Δpeak **−12.7%** · Δins **+2.6%**（IQR 7.2，远稳于坑 140）。Soft 仍关 | `a1_p2_lockless_beta.sh` · `VERDICT_P2_LOCKLESS_BETA.md` · `c6` |
| **142** | NOTE | P2 税刀后门禁+peers 重跑 | verified：A2 平/降、A3 峰平；sync 交错 **insert +16.5% 变慢**（update +11%→噪声）⇒ **勿升 Soft**。peers：空间 vs SQL/LMDB 不变；async 写碾压；sync both/SQL 写仍 ≤1× 量级 | `soft-a1-p2-gate` · `peers-p2` · `VERDICT_P2_GATE.md` · `PEERS_P2.md` |
| **143** | FIXED | Soft sync insert「税」实为休眠噪声 | verified：50k Soft sync **fits=prom=parts=0**；旧门禁 +16.5% 贴噪声带。**刀**=`part_cap_need` 不再扫 `largest_hole`（看 `job->starve_tight`）；门禁×9 + 休眠门槛。重跑 insert **+1.0% 噪声** ⇒ **可议升 Soft**（仍未默认开） | `c6` · `gate_p2_a2a3_lat.sh` · `VERDICT_P2_GATE.md` |
| **144** | NOTE | P2 显式升 Soft 默认 | verified：用户拍板；`configure_deploy` 开 starve_fit+promote。门禁/peers 重跑见 `VERDICT_P2_SOFT_ON.md` · `PEERS_P2.md` | `c6` · `soft-a1-p2-gate` · `peers-p2` |
| **145** | NOTE | Soft sync range vs LMDB 缝 / 读刀消融 | verified：**根**=未整理多 run（50k ~10 run · ~12 cursors/scan；polish→1 run · ~1.25）。cheap/cliff/lvl_t4/heap/k1 **无稳定 range 胜**；家族链 Soft 像上中性、**保留**（merge 同口径）。**勿**默改合并换读。全文 `backup/range-opt/VERDICT_RANGE.md` | `ablate_range_*.sh` · `range_knives.sh` · `Cursor::prime` 链 |
| **146** | NOTE | Soft range 根源诊断（路径 C） | verified：终态卡 **`skip_need_t`**（fam&lt;lvl_t）；50k L0 fam=6；200k 顶层 runs≈25 但 **fam=1**（P2 片）。cliff1/cheap_inf 终态无改；lvl_t4 cursors 可升。polish 唯一稳收 cursors→~1.3。**leading 下一刀=B（整理一等）非 A（热拧 cliff/cheap）**。全文 `DISCUSS_RANGE_ROOT.md` | `range_root` · `set_merge_diag` · `fam_hist` |
| **147** | NOTE | 路径 B `soft_consolidate` 可选项 | verified：API 默认 **关**；Soft+cons 收 cursors（50k 12→2.3；200k 6.5→1.2）但 200k insert **+8×**、peak_x **5.2** ⇒ **勿升 Soft 默认**。升门禁见 DISCUSS §4.2 / CONSTRAINTS。回归 `g18` t_tidy_sm | `set_soft_consolidate` · `ablate_range_b.sh` · `g18` |
| **147b** | NOTE | 动态整理消融 | verified：写中 duty8/32/hyst 在 200k **cursors 升至 15–32**（差于 Soft）；**post（写后≤32 步）** 写税≈Soft、cursors≈1.3。eager 仍炸峰。leading=写后/空闲，非写路径 duty。全文 `DISCUSS_RANGE_DYNAMIC.md` | `ablate_range_dyn.sh` |
| **148** | NOTE | Soft sync 工作节奏 | verified：200k 峰在 **upd Q4**（最长 PEAK 串 20；max commit 249 ms@c569，level 主导）；谷在 **ins 前中**（VALLEY 串~7）。PEAK≈10%/VALLEY≈25%（相对分位）。全文 `DISCUSS_SYNC_WORK_RHYTHM.md` | `sync_work_trace.sh` · `analyze_sync_work.py` |
| **149** | NOTE | Idle/Post 动态整理（偷谷） | verified：Idle=谷串+冷却；Post=边沿负债+谷还。写税刀 `min_gap=32`/`run_hi=16`：200k **Post insert +30%** cursors **1.5**；Idle 过限频会 cursors 升。**勿升 Soft 默认**。peers Soft sync+Post：vs SQL range **2.68→0.50×**（写税 insert **1.78×** base）；vs LMDB range **13→2.5×**、space **0.47×** LMDB。async+cons 上 Post≈中性。回归 `g18` | `SoftCons::Post` · `peers_post_ab.sh` · `backup/peers-post/PEERS_POST.md` · `DISCUSS_RANGE_IDLE.md` |
| **150** | FIXED | Post 写路径谷还仍交 insert 税 | verified：谷还=写中 tidy。改 Post 写中只记债；`soft_cons_repay`=consolidate→relocate→checkpoint→shrink。消融 insert≈Soft、cursors~1.3。**升默见 151** | `soft_cons_repay` · `g18` · `ablate_range_idle` · `PEERS_POST.md` |
| **151** | FIXED | Soft 默认升 Post | verified：门禁可议通过 — 消融 200k insert **0.98×** cursors **1.30**；Soft A1 peak **+0.4%** insert **+12.4%**；peers sync range SQL **3.97→0.46×**。`configure_deploy`/Soft → Post；读前 `soft_cons_repay`/`polish`；消融 Off | `VERDICT_POST_SOFT_ON.md` · `soft_post_on_verify.sh` · `g18` |

## 新坑模板

```markdown
| NNN | OPEN | <症状一句话> | leading: <假说> | — |
```

关闭时改为 verified 原因 + 做了哪些排除检查，并填「回归 / 探针」列；产品代码加 `// 坑 NNN`。
