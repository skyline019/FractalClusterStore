# Soft A1 · P1 `protect_stream_small` 消融判决

> 命令：`make soft-a1-p1`（N=200000 B=500 VSZ=32 quiet=1 inv=0）  
> 日志：`soft_a1_p1_off.log` / `soft_a1_p1_on.log` · 坑 132

## 数字

| 臂 | peak_tail | peak_x | end_x | HOLE_STARVE | PRE_DIRTY | hole_protect_skips | hw_reclaims |
|----|-----------|--------|-------|-------------|-----------|--------------------|-------------|
| **off**（Soft 默认） | 3354 | **1.804** | 1.455 | 12 | 0 | 56 | 17 |
| **on** | 7759 | **4.151** | 1.582 | 2 | 126 | 7306 | 967 |

## 判决

**拒绝升 Soft 默认。** 机制按设计生效（skips 56→7306，HOLE_STARVE↓），但 Soft A1 峰 **+130%**，归因从 HOLE_STARVE 翻成大量 PRE_DIRTY + 尾部疯长（峰时 maxh=3846 仍不复用）。

leading：小流式（含合并片边角/续租）被拦出大洞后改吃尾 ⇒ \(F_{\mathrm{reuse}}\) 更差，与 P1 命题反号。

## 产品面

- `configure_deploy` **不**开 `protect_stream_small`
- 旋钮保留可消融；下一刀须收窄谓词（勿拦合并产物尺度）或改预留，再跑本表
- 三画像未跑 on 臂（峰已否，无需 A2/A3）

## 机制回归

`c4` 仍覆盖：默认 stream 可吃洞；P1 开时 np&lt;floor 跳洞、np≥floor 仍吃；Soft 默认关。
