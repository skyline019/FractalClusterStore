# 并发错峰 A：双 MemTable / `flush_mem` 出锁（可选 · 消融）

> **状态：** S10 判决（2026-11-05）— **Negate-keep**；**可选**；**Soft 默认关**；勿升 `configure_deploy`。peers：`PEERS_DUAL.md`。  

> **坑号：** 156。  
> **前序：** [`DISCUSS15_FSYNC.md`](DISCUSS15_FSYNC.md) §3.5；节奏 [`DISCUSS_SYNC_WORK_RHYTHM.md`](DISCUSS_SYNC_WORK_RHYTHM.md)；整理错峰已落地 Post（坑 150/151）。  
> **轨道：** **延迟 / 锁持有**（读者 max、多写者重叠），**不是**空间峰 KPI（CONSTRAINTS：勿把 dual-memtable 当空间 A/B 胜者）。

---

## 1. 命题（错峰映射）

| 今日 | 错峰目标 |
|------|----------|
| `commit_phase1` 持 XLock 做完整 `flush_mem`（编码 + 发写） | **峰**（编码/IO）移出 XLock |
| 读者 / 第二写者在 phase1 被挡 | 锁内只留 **指针交换 + 账**（µs 级「谷」） |
| Post 整理已「写记债、读还债」 | A 刀正交：错开的是 **提交锁内 CPU/IO**，不是 consolidate |

一句话：把「编码 imm」当成相对 mem 写入的 **峰**，用双表把峰挪到锁外；与 SoftCons::Post 的时间轴错峰同构、不同轴。

---

## 2. 目标语义（不变式）

1. **可见性：** `get` / 游标仍见「最新已提交逻辑视图」；`imm_` 在飞期间 = 刚切出、尚未入账的那批（与今日 phase1 末「mem 已清、run 已入账」的时序需严格对齐，见 §4）。  
2. **持久性：** `commit(true)` 返回 ⇒ 仍保证持久；阶段二/三代数与 `cmu_` 不变。  
3. **失败：** 锁外 flush 失败 ⇒ `dirty_` + 按盘重建；不得留下「mem/imm 双活且盘旧」的静默错读。  
4. **单进程：** 不引入跨进程协议。

---

## 3. 机制草案（经典 imm）

```
phase1 (XLock):
  if dual_mem off:  // 今日路径
    flush_mem → 清单/SB 抓票 …
  else:
    swap: imm_ ← mem_; mem_ ← 新空表     // µs
    // 可选：仅挂「待编码」标记；清单/SB 延后到 imm 入账后
    发票 / 或发「半票」见 §4.2

锁外:
  encode(imm_) → 页写发写
  wait 本批页（或并入既有 wait_io）

再拿 XLock:
  清单入账 + 清 imm_ + 抓 SB 载荷（若未抓）
  → phase2 / phase3 同今日
```

### 3.1 读路径

`get_at` / 游标：`mem_` → `imm_`（若非空）→ 段。  
`imm_` 寿命 ≈ 一次 flush；多一次哈希。

### 3.2 必须改动的钩子（DISCUSS15 §3.5）

| 点 | 风险 | 处理方向 |
|----|------|----------|
| `job_overwrite` 在 `flush_mem` 内按条结算 | 锁外扫描 | imm 只读扫 + 锁内一次结算 |
| NOSPACE / retire 中间提交 | 必须持锁 | 退回锁内路径或 dual 关时走老路 |
| `commit(false)` async 批 | 两表合计 `mem_bytes` | 阈值按 `mem_+imm_` |
| `rollback` / `drain_inflight` | imm 在飞 | rollback 前等 imm 落或丢弃策略写死 |
| 游标 `mem_sorted` | 两表合并序 | 显式双路归并或物化视图 gen |

---

## 4. 开放设计点（实现前必须拍板）

### 4.1 可见性切点（二选一）

| 方案 | 读者何时看到新键 | 复杂度 |
|------|------------------|--------|
| **V1 入账后可见** | imm 编码+清单入账后再从段读；锁外期间仍读旧段+mem（新 put 进新 mem） | 与今日「phase1 末即见」略有窗口差 |
| **V2 imm 立即可见** | 切表后 get 查 imm | 需保证 imm 与段不双计；失败回滚难 |

**建议默认 V1**（失败面小）；消融可试 V2 仅实验。

### 4.2 与 phase2 的耦合

- **窄 A（推荐第一刀）：** 仅把 `flush_mem` 的编码+pwrite 移出锁；清单入账仍在「再拿锁」后、SB 抓票仍在入账后 → phase2 不变。  
- **宽 A：** 与组提交/合并 SB 一起做 → **本探讨不做**（DISCUSS15 §4.5）。

### 4.3 与 Soft / Post

- **Soft 默认：关 dual_mem。**  
- Post repay / polish 不依赖 dual_mem。  
- 勿在未过门禁时塞进 `configure_deploy`。

---

## 5. API（可选 · 消融）

```cpp
void set_dual_mem(bool on);   // 默认 false；不进 Soft
bool dual_mem() const;
// 观测（建议）
u64  dual_mem_flushes() const;
u64  dual_mem_lock_us() const;   // phase1 锁内墙钟（探针）
```

回归：`g18` 或 `c7`：默认关；开时单线程正确性 + 多写者 smoke。

---

## 6. 消融与门禁（升 Soft 前必须）

**KPI 轴 = 延迟，标 KPI_UNVERIFIED 直到跑过。**

| 门 | 命令 / 画像 | 通过草案（可改 CONSTRAINTS） |
|----|-------------|------------------------------|
| 正确性 | `make ci-smoke` + `make test` + `make asan`（含 TSan w1/g7） | 全绿 |
| 锁内墙钟 | Soft sync 单写 · `[prof]` 或探针：phase1 锁内 | dual on 的锁内 **不含** 编码大头；相对 off **↓≥50%**（大 value 批） |
| 读者 max | `stall` 类 / g17 lockfree 精神：慢 sync 时 get | dual on **不劣于** 今日出锁基线；无回归 >20 ms 预算 |
| 写吞吐 | Soft sync 50k insert；可选 Soft async | on/off insert **≤+10%**（勿为锁指标伤写） |
| 空间 | Soft A1 峰 | **不作为升默条件**；若 on 抬峰 >5% → 记 NOTE，仍可保留为延迟可选 |
| Soft 升默 | — | **默认 Negate**：仅当多写者+读者画像有稳定胜且空间不炸 |

复现目录（落地后）：`backup/concur-imm/` · `scripts/ablate_dual_mem.sh`。

---

## 7. 切片计划（实现轮）

| 切片 | 内容 | 完成判据 |
|------|------|----------|
| **S0** | 本文 + 坑 156 + CONSTRAINTS/AGENTS 一句 | **已合** |
| **S1** | `set_dual_mem` 开关；off≡今日路径；on 仍走今日（空壳） | **已合**（`g18`） |
| **S2** | 切表 + 锁外 encode(imm) + 锁内入账；单线程 put/commit/get | **已合**（`flush_mem_dual` · `g18` `t_dual_mem`） |
| **S3** | 读路径 mem→imm→段；游标；rollback 等 imm | **已合**（V2；abort/snapshot/rollback `wait_imm_idle_`） |
| **S4** | job_overwrite / NOSPACE / async 批 | **已合**（入账仍锁内 `job_overwrite`；NOSPACE 退经典；async 合计阈值、不双切） |
| **S5** | 消融脚本 + VERDICT；决定是否保留代码 | **已合** → `backup/concur-imm/VERDICT_DUAL_MEM.md` **Negate-keep** |

每切片可合；**禁止 Soft 默认 on**（判决未过升默门）。

---

## 8. 非目标

- 多进程共享库  
- 真正的多写者并行 `flush_mem`（仍一票一 flush；错峰的是锁，不是 CPU 并行编码）  
- 用 dual_mem 追空间峰  
- 与 Post 绑死为同一旋钮  

---

## 9. 判决（S10 · tip+osz 免 probe · 2026-11-05）

| 项 | 结论 |
|----|------|
| 是否做 | **已做**（S0–S10）；代码保留为可选 |
| Soft 默认 | **关**（**Negate-keep**：缺生产长稳，非门禁红） |
| S10 机制 | S9 + **`DualTip{rid,len,osz}`**：写出时记下；settle 命中**免页探**；NIL+primed 跳过；merge bump gen → full_probe |
| S10 门禁 | write wall **~0.16×**；settle **~0.005×** off_excl；lock wall≈平；mw/get_max **PASS** |
| 形态 | `ablate_dual_mem_s10.sh` · `VERDICT_DUAL_MEM_S10.md` |
| 下一刀 | Soft 升默仅当有生产长稳；剩余 wall 大头在 encode/io/excl attach |

关联：坑 **156** · `backup/concur-imm/VERDICT_DUAL_MEM_S10.md`。
