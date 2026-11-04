# 跨学科检索 → 组合建模：峰 / 洞 / MIX（限制下）

> 状态：**探讨·建模**，不实施。  
> 目的：用近世/离散/拓扑与物理–晶格概念**列出可映射思路**，再**合成**与 Soft A1 证据一致的联合模型。  
> 前承：`DISCUSS_SPACE_MODEL.md`（约束下界）· `DISCUSS_MIX_LIVE_VIRTUAL.md` · 坑 126–130。  
> 外参：Coffman 动态分配碎片分类；LSM compaction design space；TDA 持续同调；颗粒/格子气体 free-volume；晶格缺陷语言。

---

## 1. 检索清单：概念 → FCS 像什么

### 1.1 离散数学 / 组合优化

| 概念 | 文献意象 | FCS 映射 | 对刀含义 |
|------|----------|----------|----------|
| **Partition fragmentation** | 总量够、装不进任何桶 | \(\sum F \ge \mathrm{part}\) 但 \(H_{\max} < \mathrm{part}\) | **养洞失败**的精确定义（Soft A1 +311） |
| **Checkerboarding** | 占/空交替，连续洞碎 | `free_` 直方图多小档 | hole_fit/frag A 已治一截；护洞防再碎 |
| **Internal fragmentation** | 只按规整块分配浪费 | 页原子：MIX 页整页占用 | 盐胡椒死 ⇒ 内碎片；非外洞 |
| **Bin packing / first-fit** | 请求装进洞 | `alloc_pages` 低址优先 + 护洞跳大洞 | P1：合并请求优先匹配「合并尺度」洞 |
| **Overflow ⇒ compaction** | Ting：分配聪明仍溢则搬迁最少 | NOSPACE → retire / hw / 合并 | 已有；溢时动的是所有权非抠 MIX |
| **LSM compaction design space** | trigger / layout / granularity / movement | cliff、层、分片、选哪组 run | 粒度=片；Spooky/FGC：粒度↓降 WA 升索引复杂——FCS 已 P2b |

### 1.2 拓扑 / 近世几何

| 概念 | 意象 | FCS 映射 | 对刀含义 |
|------|------|----------|----------|
| **持续同调 \(H_0\)** | 自由空间连通分支随尺度存活 | 洞尺寸过滤：小洞短寿命（并洞/吃洞），**长寿命大洞**才服务合并 | 观测：barcode = 洞寿命；KPI 应用「寿命≥part 的 \(H_0\) 条数」 |
| **持续同调过滤** | 半径↑，孔生灭 | 尺度 \(\varepsilon\)=页数；\(\beta_0(\varepsilon)\)=≥ε 的洞个数 | Soft A1：\(\beta_0(64)\) 在涨尾时常 →0 |
| **配置空间 / 硬球** | 厚度物体在窄带里能否通过 | 「片」=硬球直径 part；带=线性地址 `[2,T)` | **几何阻塞**：产物穿不过洞喉 ⇒ 长尾（=卡死在配置空间连通分支外） |
| **同伦 / 形变收缩** | 连续形变不撕裂 | 维护重写=形变；热读路径禁止形变 | I2：读侧合同伦于「单页纤维」 |

### 1.3 晶格 / 固体物理（缺陷语言）

| 概念 | 意象 | FCS 映射 | 对刀含义 |
|------|------|----------|----------|
| **空位 vacancy** | 晶格缺原子 | `free_` 洞 | 可复合并「原子团」（片） |
| **间隙原子 interstitial** | 挤进非格点 | **PEND**：已离开 run 晶格、未入 free 气体 | 峰的「过饱和间隙」=987 页 |
| **替位 / 合金无序** | 格点种类混 | **MIX 页**：活/死记录共格 | 有序化=重写/合并（退火） |
| **位错 / 钉扎** | 线缺陷钉住滑移 | TRIM **page0 锚**；快照 deferred | 锚成本 1 页/源；钉扎期峰值收益冻结（P5） |
| **再结晶** | 新晶粒吞旧晶粒 | 层合并：新 L3 晶粒，旧 L2→PEND→溶解 | 峰=新旧晶粒共存窗口 |

### 1.4 统计物理 / 颗粒体 / 玻璃

| 概念 | 意象 | FCS 映射 | 对刀含义 |
|------|------|----------|----------|
| **Free-volume 动力学约束** | 无洞则粒子不能动 | 无 ≥part 洞 ⇒ 合并产物不能「就地落」 | 与护洞目标一致：保留 free volume 给合并 |
| **Jamming / 堵塞** | 密度高动不了 | 养洞失败 + 同提交 P 钉死 = **堵塞峰** | 解堵=开喉（洞）或错时（跨提交） |
| **Tetris / 振实对数松弛** | 轻敲→慢密实 | Soft maint 小预算=弱振；polish=强振 | 弱振不解堵几何；强振伤延迟 |
| **Kob–Andersen 格子气** | 邻居数约束才可跳 | 「片」移动约束=洞几何+非护洞路径 | 约束过严（L0 吃洞）⇒ 有效温度再高也 jam |

### 1.5 重正化 / 多尺度（近世场论意象）

| 概念 | 意象 | FCS 映射 | 对刀含义 |
|------|------|----------|----------|
| **粗粒化 RG** | 积掉短波自由度 | L0→L1→L2→L3 合并 | 每步积掉 MIX 盐粒，导出 WA |
| **相关长度 \(\xi\)** | 有序畴尺度 | 最大可用洞；或家族键域宽度 | \(\xi < \mathrm{part}\) ⇒ 临界以下，必须长尾 |
| **固定点** | 流终点 | Soft 稳态：稀疏 MIX + 有护洞 | 峰是**瞬态偏离**固定点（合并窗口） |
| **Fine vs coarse compaction** | FGC/Block/Spooky | P2b 片 = 细粒度；全量 consolidate = 粗 | 已选细；残差不在粒度而在 **几何喉** |

### 1.6 量子意象（严格：隐喻，非物理主张）

| 隐喻 | 用法 | 禁止误用 |
|------|------|----------|
| **叠加** | MIX 页 = 活\|死 未投影到「全死可回收」 | 不是真量子；不能「测量塌缩」代替 recount |
| **测量** | `exact_live_recs` / recount = 投影到存活账 | 热路径禁止频繁测量 |
| **隧道** | promote_pending：势垒=SB；一过则 P→F | 不能无 SB「隧穿」复用（耐久） |
| **纠缠** | seg 跨页共享边界页 | 邻单元共享页 ⇒ 不能单边 HOLE（P2c 设计已限内部整页） |

---

## 2. 组合建模：FCS 晶格–渗流–重正化三联体

把检索结果收成**一个**状态机模型（与 `DISCUSS_SPACE_MODEL` 同构，换语言便于推导）。

### 2.1 基底：一维晶格 \(\Lambda = \{2,\ldots,T-1\}\)

每格点状态（互斥，与 LIVE_INV 一致）：

\[
\sigma_i \in \{\mathrm{OWN_{live}}, \mathrm{OWN_{mix}}, \mathrm{OWN_{dead}}, \mathrm{META}, \mathrm{VAC}(F), \mathrm{INT}(P), \mathrm{PIN}(D)\}
\]

- Soft A1 峰：\(\mathrm{OWN_{dead}}\approx 0\)，\(\mathrm{INT}\) 大，\(\mathrm{VAC}\) 中等但 **渗流簇直径** \(H_{\max}\) 常 < part。

### 2.2 序参量

| 序参量 | 定义 | 峰失败判据 |
|--------|------|------------|
| \(\rho_{\mathrm{live}}\) | 逻辑活 / T | peak_x ↑ ⇔ ρ↓ |
| \(\phi_{\mathrm{vac}}\) | F/T | 总量够不够 |
| \(\xi_{\mathrm{hole}}\) | 最大 VAC 连通长度 \(H_{\max}\) | **\(\xi < \mathrm{part}\) ⇒ 几何 jam** |
| \(m_{\mathrm{mix}}\) | 记录死分数 | 稳态税；峰时小 |
| \(n_{\mathrm{int}}\) | P/T | 瞬态间隙原子密度 |

**主临界条件（组合核心）**：

\[
\boxed{\xi_{\mathrm{hole}}(t) \;\ge\; \mathrm{part\_cap}(t)
\quad\text{（合并/分片可落）}}
\]

否则产物激发「尾部生长模」\(\Delta_{\mathrm{prod}}\)。Soft A1 插入期反复违反 ⇒ +311 链。

### 2.3 动力学通道（算符）

| 算符 | 物理/晶格名 | FCS | 守恒/破缺 |
|------|-------------|-----|-----------|
| \(\mathcal{F}\) flush | 吸附 + 替位无序↑ | 新 OWN，旧页 MIX↑ | 破 ρ 局部 |
| \(\mathcal{M}\) merge | 再结晶 / RG 一步 | 新 OWN；源→INT | 瞬态 \(n_{\mathrm{int}}\)↑ |
| \(\mathcal{P}\) promote | 间隙→空位（退火） | P→F | 需 SB「激活能」 |
| \(\mathcal{A}\) alloc | 空位凝聚/吞噬 | VAC→OWN | 护洞=选择规则 |
| \(\mathcal{R}\) rewrite | 有序化退火 | MIX→LIVE 新晶粒 | 空闲时；有 WA |

**堵塞（jamming）态**：\(\mathcal{M}\) 需要 \(\xi\ge\mathrm{part}\)，但 \(\mathcal{A}_{\mathrm{user}}\) 持续把大 VAC 切碎 / 占尾 ⇒ \(\xi\) 维持亚临界。

### 2.4 拓扑观测与控制

定义过滤 \(X_\varepsilon = \{i:\sigma_i=\mathrm{VAC},\;\text{连通长}\ge\varepsilon\}\)。

- \(\beta_0(\varepsilon)=\#\) 连通分支。  
- **控制目标**：维持 \(\beta_0(\mathrm{part})\ge 1\) 于合并窗口（至少一个「可通喉」）。  
- 护洞 = 保护大分支不被 user \(\mathcal{A}\) 切碎（拓扑上保护长 barcode）。

### 2.5 与限制下彻底解的对齐

| SPACE_MODEL 相位 | 本模型语言 |
|------------------|------------|
| P1 洞接片 | 维持 \(\xi\ge\mathrm{part}\)（渗流超阈） |
| P2 P 错开 | 降低 \(\mathcal{M}\) 与 \(\mathcal{P}^{-1}\) 时间重叠（减 INT 过饱和） |
| P3 dense 重写 | 对高 \(m_{\mathrm{mix}}\) 晶粒退火 |
| P4 P2c | 仅 \(\mathrm{OWN_{dead}}\) 整格点→VAC（真空位，非 MIX） |
| 合法剩余 | I1 盐粒 MIX；I4 最小 INT 脉冲 |

---

## 3. 思路组合矩阵（可选用，默认仍关）

| 组合 | 成分 | 期望效应 | 风险 |
|------|------|----------|------|
| **G1 护喉** | 晶格空位尺度保护 + bin packing | \(\xi\) 超阈 | L0 更碎/长尾（须消融） |
| **G2 振实错相** | 颗粒弱振=预算；与 \(\mathcal{M}\) 错相 | INT 峰值↓ | 延迟 |
| **G3 RG 粒度已定** | 细粒 P2b | 已收 | 勿再无几何地加细 |
| **G4 缺陷退火** | dense MIX \(\mathcal{R}\) | 合前 ρ↑ | WA |
| **G5 拓扑监控** | \(\beta_0(\mathrm{part})\) 进探针 | 可证伪 P1 | 仅观测 |

**不组合**：量子转发活链（破 I2）；把 MIX 当 \(\mathrm{OWN_{dead}}\) 挖洞（拓扑上洞未形成）。

---

## 4. 一句话合成

> FCS 地址空间是一维**硬球配置 + 晶格气体**：活数据是粒子，洞是 free volume，PEND 是间隙原子，MIX 是替位无序；层合并是再结晶/粗粒化。  
> **峰 = 几何堵塞（喉 \(\xi<\)part）+ 间隙过饱和（INT）**；不是量子叠加也不是单纯密度不足。  
> **彻底（限制下）= 让空位渗流簇在合并窗口保持超临界，并让间隙原子脉冲与产物生长错相**；盐粒无序只作次级退火。

---

## 5. 参考文献锚（检索入口，非背书实现）

- Coffman et al.：动态存储三类碎片（partition / checkerboard / internal）。  
- LSM compaction design space / Compactionary；Spooky、FGC（粒度）。  
- Persistent homology 入门（Edelsbrunner–Harer；多尺度孔寿命）。  
- 颗粒 free-volume / Tetris 振实；Kob–Andersen 动力学约束。  
- 晶格缺陷教科书语言（vacancy / interstitial / dislocation）作**工程隐喻**。

复现观测建议：在 `soft_a1` / layout 打印 \(\xi=H_{\max}\)、\(\beta_0(64)\)、\(n_{\mathrm{int}}=P/T\) 时间序列，对照涨尾事件（已有 ATTR=HOLE_STARVE 即亚临界 \(\xi\)）。
