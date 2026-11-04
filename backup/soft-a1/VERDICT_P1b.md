# Soft A1 · P1b `protect_stream_small` anti-shatter 消融判决

> 命令：`make soft-a1-p1`（N=200000 · inv=0）· 坑 133  
> 对照薄 P1（整洞跳过）：`VERDICT_P1.md`

## 数字

| 臂 | peak_tail | peak_x | end_x | HOLE_STARVE | PRE_DIRTY | skips | hw_reclaims |
|----|-----------|--------|-------|-------------|-----------|-------|-------------|
| **off** | 3427 | **1.860** | 1.461 | 12 | 1 | 44 | 19 |
| **on（anti-shatter）** | 4097 | **2.200** | 2.088 | 1 | 16 | 1954 | 712 |
| 参考薄 P1 on | 7759 | 4.151 | 1.582 | 2 | 126 | 7306 | 967 |

## 判决

**拒绝升 Soft 默认。** P1b 比薄 P1 轻（2.20 vs 4.15），仍相对 off **+18% peak_x**，end_x 明显变差；skips/hw 暴涨说明切碎守卫仍常迫使改道。

leading：合并流式仍频繁碰到「切完残桩 &lt; floor」的洞，跳过后改吃尾 / 触发回收连锁。

## 产品面

- Soft / `configure_deploy` **不开** `protect_stream_small`
- 旋钮 + `c4` 保留（机制正确）；下一开放自由度见 SPACE_MODEL P2（PEND 错开）或分配 best-fit，勿再升本谓词

## 机制回归

`c4`：默认可切；残桩≥floor 可切；残桩&lt;floor 跳过；整吃允许；Soft 默认关。
