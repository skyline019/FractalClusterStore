# 讨论：Harness A/B（空间峰）· GGUF 式分层卸载类比 · 稀疏紧致 vs 双 memtable

> 状态：**T 臂已消融**（坑 125）：三画像无峰增益 ⇒ Soft **默认关** `space_accel`；旋钮保留。全文结论 `backup/harness-ab-space/VERDICT.md`。  
> 相关：EVAL17 §8 · P2_DESIGN P2c · DISCUSS15 §3.5 · CONSTRAINTS · PIT_LOG 124/125。

---

## 0. 理解锁定（已吸收用户补充）

| 项 | 内容 |
|----|------|
| 主 KPI | 高压下**空间峰 / 可写性**（非延迟） |
| 画像 | **A1** layout 200k/800k 峰倍数；**A2** 小盘页预算→NOSPACE/回收后再写；**A3** Soft peers 文件占用 + `tail_/live` |
| 「动态卸载」 | 仿 **GGUF 分层 offload**：同一时间片内尽量只让「热」占用紧致物理空间，冷段可挖洞/外置/延后物化——本质仍是**空间紧致化**，不是卸进程 |
| 双 memtable | 延迟轨；本 harness **不当 A/B 冠军赛对手**；可作旁注「改动代价对照」 |
| 工程美学 | **功能密度高、少死代码、全面但简洁**；改动必须估代价：**先试 → 消融 → 再评是否留** |

---

## 1. GGUF → FCS 映射（leading，待消融验证）

| GGUF / llama.cpp | FCS 类比 | 已有 / 缺口 |
|------------------|----------|------------|
| 热层驻 GPU，冷层 CPU/磁盘 | 热 run（L0/近提交）占连续好洞；冷层允许碎/洞 | 护洞 + hole_fit + TRIM 前缀（P2b）**已有** |
| 按层粒度换入换出 | 按 run / 片 / 页区间释放中间死页 | **P2c `M_RUN_HOLE`** = 格式级「层内挖洞」（未开） |
| 同一时刻显存预算打满但不浪费 | 同一 `tail_` 预算下最大化 live 有效载荷 | A1/A2 峰倍数、NOSPACE |
| 卸载策略可配置 | Soft 旋钮 / tidy；勿堆第三套并行策略 | **T 臂**：`space_accel` 挂 Soft，无新子系统名 |

**不当映射**：把整条 Store「卸载」成另一进程；或把双 memtable 当成空间卸载（它主要搬锁，不减盘峰）。

---

## 2. 拍板（2026-04-03 用户确认）

| # | 议题 | 决议 |
|---|------|------|
| 1 | 顺序 | **R → T →（可选）P2c** |
| 2 | A2 页数 | **256 / 512 / 1024 三者都跑**（主表可并列） |
| 3 | 第一刀「动态卸载」 | **护洞**（Soft 默认保留）+ **近满加速**（已实现并消融） |
| 测 | sanitizer | 开发期 **轻量** `make asan-new`；**全量 `make asan` 本轮不做** |
| 消融 | 2026-11-04 CSV | A1/A2/A3 **峰无改善** ⇒ Soft 默认 **不开** `space_accel`；见 `backup/harness-ab-space/VERDICT.md` |
| Soft A1 | 坑 126 | `make soft-a1`：峰=养洞落尾为主，dead/live≈0.02 ⇒ **勿开 P2c**；`backup/soft-a1/VERDICT.md` |

---

## 3. Harness（方案 R）

```
make harness-ab-space          # scripts/harness_ab_space.sh
  arms: Soft(+space_accel) | 消融 space_accel=0
  profiles: A1 layout · A2 c3 tiny 256/512/1024 · A3 Soft peers 轻片段
  out: backup/harness-ab-space/results.csv
  gate: 三画像同向改善才允许「宣称为默认增益」；否则 NOTE + 可消融
```

回归测：`tests/c3.cpp`（进 `ci-smoke` / `make test`）。

---

## 4. 改动代价评估表

| 候选 | 格式/清单 | 预估热路径触点 | 死代码风险 | 消融方式 | 状态 |
|------|-----------|----------------|------------|----------|------|
| Soft 基线 + 护洞 | 无 | 0 | — | — | 已有 |
| **近满加速 space_accel** | 无 | 低 | 低 | Soft 默认关；`set_*` 实验 | **消融无峰增益** 坑 125 · 旋钮保留 |
| P2c 稀疏 run | **有** `M_RUN_HOLE` | 中高 | 高若半吊子 | 编译期关 op | 峰仍卡再开 |
| 双 memtable | 无 | **很高** | 双路径易腐 | 难 | **本轨不做** |

---

## 5. 与「功能密度」规则对齐

1. 新臂若消融后增益 < 噪声或仅单画像有效 → **删代码**，只留 NOTE。  
2. 禁止并行三套「整理哲学」；空间紧致挂在 Soft / 现有 maint。  
3. 宣称「峰下降」必须附 A1+A2+A3 数字或标 `KPI_UNVERIFIED`。
