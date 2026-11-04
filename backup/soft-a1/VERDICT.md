# Soft A1 峰值归因（坑 126）· 落地

> 命令：`make soft-a1` → `backup/soft-a1/soft_a1_200k.log`  
> 配置：`configure_deploy(false,150)` Soft（护洞+hw150+frag；`space_accel=0`）  
> 跑次：2026-11-04 · `peak_tail=3428` · `peak_x=1.860` · `end_x=1.461` · `live_logic=1843`  
> （与 harness A1 Soft-ish 1.860× / 3428 **同数**）

## 轨迹要点

| 阶段 | 现象 | ATTR |
|------|------|------|
| ins #65…#385 | 每 ~64 提交 **+311** 落尾；`maxh=29–40 < 64`；`pend=328` | **HOLE_STARVE** ×6 |
| upd #451…#549 | 继续 +100–289；maxh 仍常 <64 | **HOLE_STARVE** 为主 |
| upd #569（**峰**） | 3324→**3428** (+104)；`r0=1.333`；`pend=987`；L2=0 L3=22；`maxh=102` | **PRE_DIRTY**（合并波）+ 高 pend |

`ATTR_COUNTS`：big=13 · HOLE_STARVE=12 · SB_LAG=10 · PRE_DIRTY=1

## 分解（leading → Soft A1 复验）

1. **养洞 / 中等产物落尾（主残差）** — 插入期固定 +311、洞装不下片（ROUND17 §2.5 轨迹同构）。  
2. **合前占用** — 峰前一跳 `r0=1.333`；不是「从 1.0 突然翻倍」。  
3. **SB / pending 放大** — 峰时刻 `pend=987`；洞已有 102 仍涨尾（复用未就绪）。  
4. **run 内死页** — 峰时刻 recount 后 `dead_est/live=0.023` ⇒ **P2c 门不开**（死页解释不了 ~0.8× 残差）。

## 门控

| 候选 | 结论 |
|------|------|
| `space_accel` | 已消融；本跑 Soft 默认关 |
| **P2c `M_RUN_HOLE`** | **勿开**（峰时 dead sparse；合前脏 L2 已走 PEND 整块回收） |
| 下一刀 | 分配器/养洞：L0/L1 少吃合并可用洞，或片与洞更匹配（须单独消融） |

配套：`ALIVE_DEAD.md`（页类）· `FRAG_DEAD.md`（死碎片分布/机制）

## 复现

```bash
cd embdb/x && make soft-a1
# 或：./build/soft_a1 200000 500 32 1
```
