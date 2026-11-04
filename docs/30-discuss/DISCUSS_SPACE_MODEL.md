# 建模：限制下如何「彻底」解决空间峰 / MIX

> 状态：P1d 消融最优 **mode=2+skip180**（`VERDICT_P1d.md`）；P2 Soft A1 **fit+promote** 峰↓≈8%；门禁 A2↓/平、A3 平、update+16% ⇒ **Soft 勿升**（`VERDICT_P2_GATE.md`）。开放：降写税后再议升默认。  
> 目的：把「彻底解决」改写成**约束可行域上的可达成下界**，避免无界理想解。  
> 证据锚：Soft A1 · layout A1 · harness · EVAL17/ROUND16–18 · 坑 125–134。  
> 跨学科组合（晶格/渗流/拓扑/RG）：`DISCUSS_SPACE_MATHPHYS.md`。

---

## 0. 「彻底」在限制下的定义

无约束的「彻底」= `peak_x → 1` 且 `MIX → 0` 且延迟不变 —— **与追加写不变量矛盾**。

**约束下的彻底**（本文采用）：

> 在硬不变量集合 \(\mathcal{I}\) 内，将目标推到**由 \(\mathcal{I}\) 推出的可达下界** \(L^*\)，并证明剩余缝隙只能来自已标明的不可消除项或未闭合的自由度；每一刀可消融、有画像门禁。

即：不是消灭所有死与峰，而是 **没有「还允许动且未动」的杠杆**。

---

## 1. 状态变量（一页纸）

在提交点 \(t\)（或维护单元边界）定义：

| 符号 | 含义 | Soft A1 峰量级 |
|------|------|----------------|
| \(T\) | `tail_`（高水位） | 3428 |
| \(L_{\mathrm{phys}}\) | `live_page_count` = \(T - |free|\) | ~2015 |
| \(L_{\mathrm{logic}}\) | `live_pages()`（归一口径） | 1842 → **peak_x = T/L_logic ≈ 1.86** |
| \(F\) | `free_` 页 | 536 |
| \(P\) | `pending_free_` 页 | **987** |
| \(D\) | `deferred_`（快照） | 0 |
| \(R_{\mathrm{own}}\) | run owned 数据+meta（含 MIX） | ~1750 data |
| \(M_{\mathrm{dead}}\) | 记录级死 / nrec | 峰 3%；合前 L2 25% |
| \(H_{\max}\) | 最大洞 | 峰时 102；养洞失败时常 <64 |

恒等式（忽略 man/ovfl 小项）：

\[
T \approx R_{\mathrm{own}} + F + P + D + O(\mathrm{man})
\]

峰 KPI：

\[
\mathrm{peak\_x} = \frac{\max_t T(t)}{L_{\mathrm{logic}}^{\mathrm{end}}}
\]

---

## 2. 硬不变量 \(\mathcal{I}\)（不可破，否则换产品）

| ID | 不变量 | 直接推论 |
|----|--------|----------|
| I1 | **追加写 / 不可变 run**（flush 出新页） | 覆盖 ⇒ 旧页留死记录 ⇒ **MIX 下界 > 0** 除非重写 |
| I2 | **热 get ≤ ~1 数据页**（无键级转发） | 禁止持久活链追跳 |
| I3 | **rid = page0 身份锚** | TRIM 须留锚页；不能「整 run 物理蒸发」无记账 |
| I4 | **释放经 SB 闸**（`pending_free_`） | 合并瞬间 **P 下界 ≈ 本批源块**（单提交归并时尤硬） |
| I5 | **用户写不吃护洞大洞**（Soft） | L0/flush 与合并争洞 ⇒ 养洞张力 |
| I6 | **单进程 · u64 键 · Soft 默认面** | 禁 SQL/多进程/默默第三套整理哲学 |
| I7 | **默认可消融 + 三画像同向才升** | 无证刀不得进 Soft 默认 |

I1+I2 ⇒ 「无重写、无转发、零 MIX」**空集**。  
I4 ⇒ 「合并中 P=0」**空集**（除非合并跨多提交且源已 promote——有预算/延迟代价）。

---

## 3. 目标分解（可达下界）

把峰分解为**可加项**（leading，Soft A1 轨迹校准）：

\[
\underbrace{T_{\mathrm{peak}}}_{3428}
\approx
\underbrace{L_{\mathrm{pre}}}_{\text{合前活+脏占用}}
+ \underbrace{\Delta_{\mathrm{prod}}}_{\text{产物落尾/占洞}}
+ \underbrace{P_{\mathrm{src}}}_{\text{源未 promote}}
- \underbrace{F_{\mathrm{reuse}}}_{\text{当场复用的洞}}
\]

对应 Soft A1：

| 项 | 机制 | 下界 \(L_i^*\)（限制下） | 今日缝隙 |
|----|------|-------------------------|----------|
| \(L_{\mathrm{pre}}\) | 合前 run 占用（含 MIX） | \(\approx L_{\mathrm{logic}}\) + 不可避免的层缓冲 | 时机旋钮已否；dense 可用空闲重写逼近 |
| \(\Delta_{\mathrm{prod}}\) | 片/中等产物 | \(\approx\) 一片（P2b 意图） | **养洞失败** ⇒ 整片/中等产物长尾 |
| \(P_{\mathrm{src}}\) | I4 | \(\ge\) 本批源页（同提交） | 跨提交分片可降**瞬时**叠峰，预算 0 全挤一次会更差（R17 坑） |
| \(F_{\mathrm{reuse}}\) | 洞 ≥ part | 洞几何匹配片 | maxh≪part ⇒ reuse≈0 |

**MIX 子模型**（稳态税，非峰主项）：

\[
M_{\mathrm{dead}}(t) \approx \underbrace{u\cdot\alpha}_{\text{点覆盖率}} \cdot \mathbf{1}_{\text{未重写}}
\]

- 随机覆盖 ⇒ 页内均匀、短 streak（已证）。  
- 层合并：整 run 重写 ⇒ \(M\) 重置，源进 \(P\)（死从「页内」变成「水位假占用」）。

**限制下彻底解峰** ⇔ 同时逼近：

1. \(\Delta_{\mathrm{prod}} - F_{\mathrm{reuse}} \to 0\)（**洞接住每一片**）  
2. \(P_{\mathrm{src}}\) 与 \(\Delta_{\mathrm{prod}}\) **时间错开**（跨提交 promote，且不伤延迟预算）  
3. \(L_{\mathrm{pre}}/L_{\mathrm{logic}} \to 1+\varepsilon_{\mathrm{level}}\)（层必要缓冲）

**限制下彻底解 MIX** ⇔ 接受稀疏盐粒；对 dense 用空闲重写逼近 \(M\to 0\) 于该 run；全死页才 P2c。

---

## 4. 可行域：已闭合 vs 仍开放

```
                    ┌─ I1 追加写 ─────────────────────────┐
                    │  MIX≥盐粒 除非 maint 重写            │
峰 T ───────────────┼─ I4 SB闸 ── P≥源（同提交叠峰）───────┤
                    │  开放：跨提交错开 / 更快 promote*     │
                    ├─ 洞几何 ── Δ_prod 可被 F 吃掉 ───────┤
                    │  开放：分配器养洞（护洞已开，仍失败） │
                    └─ I2 单页读 ─ 禁转发 ─────────────────┘
```

\*「更快 promote」受 I4 语义约束：只能 **多提交边界**，不能偷 SB。

| 杠杆 | 状态 | 闭合理由 / 开放点 |
|------|------|-------------------|
| space_accel | **闭合（无效）** | 三画像消融 |
| cliff/dirty 默认 | **闭合（有害）** | EVAL17 |
| coalesce B | **闭合（抬峰）** | R18 |
| 护洞 Soft | **保留** | 机制正确，200k KPI 弱 |
| hole_fit / frag A | **已收一刀** | 残差=养洞 |
| P2c 打峰 | **闭合（不对症）** | 峰 DATA_DEAD=0 |
| **分配器：合并洞不被 L0/L1 啃光** | **半开（P1d 最优臂实验）** | 坑 135：mode2+skip180；Soft 勿升；可转 P2 |
| **分片跨提交 + 预算（P2 starve）** | **半开（Soft A1 both）** | 坑 138：fit+promote 峰↓；勿升 Soft；A2/A3 开放 |
| **空闲 dense-run 重写** | **开放（薄）** | 降合前/ MIX 税，不保证峰 |
| 键级活链 | **闭合（禁）** | I2 |

---

## 5. 彻底方案 = 有序闭合程序（模型解，非开工令）

在 \(\mathcal{I}\) 下，**完整解**是一条必须跑完的程序；跳步 = 未彻底。

### Phase P0 — 定义下界与度量（已有）

- peak_x、PEND、maxh、FRAG dens、三画像 CSV。  
- 成功：残差可归因到表 §3 某一 \(L_i^*\)。

### Phase P1 — 闭合 \(\Delta_{\mathrm{prod}}-F_{\mathrm{reuse}}\)（峰主残差）

**命题**：若每次分片/中等产物落地时 \(\exists\) 洞 \(\ge \mathrm{part\_cap}\)，则该项 → 0。  
**手段**：P1c（坑 134）`set_stream_best_fit` — 流式最小适配洞；`configure_deploy` 不开。  
**消融**：Soft A1 / Layout A1 **峰降**（`VERDICT_P1c.md`）；升默认待 A2/A3。跳洞类 P1/P1b 已否。  
**回归**：`c5` · `make soft-a1-bf` · layout argv[14]。

### Phase P2 — 闭合 \(P\) 与产物的时间交叠

**命题**：源页在产物下一片之前进入 `free_` ⇒ 叠峰 ↓。  
**手段**：已有分片+跨提交；调预算/units，避免 R17「一提交挤完 → P 钉死」。  
**证伪**：错开后 peak_x 不降或 get/commit 延迟越线。  
**性能**：显式延迟–空间帕累托，越线则不升默认。

### Phase P3 — 闭合 \(L_{\mathrm{pre}}\) 中可避的 dense MIX

**命题**：合前 \(M_{\mathrm{dead}}\) 高的 run 在空闲时重写 ⇒ \(L_{\mathrm{pre}}\) ↓ 且后续 \(P\) 更小。  
**手段**：B2 机会重写（DISCUSS_MIX）；默认关。  
**证伪**：只降 FRAG、peak_x/peers 无益或变差 → 删。

### Phase P4 — 闭合真全死页（可选）

**命题**：稳定 `DATA_DEAD>0` ⇒ P2c 回收。  
**手段**：`M_RUN_HOLE`。  
**证伪**：仍无 DATA_DEAD → 不开。

### Phase P5 — 证明「已彻底」

同时满足：

1. Soft A1：\(\mathrm{peak\_x} \le L^*_{\mathrm{model}} + \eta\)（\(\eta\)=噪声；\(L^*\) 由 P1–P2 达到后标定）。  
2. A2/A3 不恶化。  
3. peers 不劣。  
4. 开放表 §4 无未尝试的「开放」项；闭合项有消融记录。  
5. 剩余缝隙书面等于：I1 盐粒 + I4 最小 P + 层缓冲 \(\varepsilon_{\mathrm{level}}\)。

若 P1–P2 后仍远大于 \(L^*\)，则 **模型缺项**（回去加变量），不是再堆虚拟化。

---

## 6. 不可达集（防止伪彻底）

| 愿望 | 为何不可达 |
|------|------------|
| peak_x=1.0 且同提交大合并 | I4：源与产物共存 |
| MIX=0 且无 maint 重写 | I1 |
| 峰降靠页内抠盐粒 | FRAG：峰时 sparse；合前 dense 已整 run→P |
| 无损 + 热路径转发 | I2 |

---

## 7. 一句话模型解

\[
\boxed{
\begin{aligned}
&\textbf{彻底}|_{\mathcal{I}}
:= \min T_{\mathrm{peak}}
\quad\text{s.t. I1–I7}\\
&\text{主自由度：洞–片匹配（P1）与 P–产物错开（P2）}\\
&\text{次自由度：空闲 dense 重写（P3）；P2c 仅 DATA_DEAD（P4）}\\
&\text{MIX 盐粒与最小 P 为 } L^*\text{ 的合法剩余，不是未完成工单。}
\end{aligned}
}
\]

下一步若开工：只开 **P1 分配器养洞消融**（最小面、对准已证残差）；P2/P3 作备选臂进同一 harness。
