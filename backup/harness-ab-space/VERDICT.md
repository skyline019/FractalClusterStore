# Harness A/B 空间峰 · 坑 125 消融结论

> 跑次：`2026-11-04` · `scripts/harness_ab_space.sh` · 原始 `results.csv`

## 数字（accel=85 vs 消融 0）

| 画像 | accel peak_x / hw | ablate | space_accels (on/off) | 同向？ |
|------|-------------------|--------|------------------------|--------|
| A1 layout 200k | **1.860** (peak 3428) | **1.859** (3427) | 0 / 0 | 无差（噪声） |
| A2 256 | 1.052 | 1.052 | 32 / 0 | 峰相同 |
| A2 512 | 1.040 | 1.040 | 64 / 0 | 峰相同 |
| A2 1024 | 1.483 | 1.483 | 14770 / 0 | 峰相同（加压猛、峰不动） |
| A3 Soft 50k | hw 3203072 B · 1.122 | 同 | 0 / 0 | 无差（未近满） |

## 结论

- **三画像均无空间峰改善** ⇒ 不得把 `space_accel` 升为 Soft 默认增益。
- leading：A2 峰被 `user_lim` 顶死，多跑 maint 不降 `peak_tail`；A1/A3 大盘从不触 85%·lim ⇒ 旋钮不发火。
- **处置**：Soft `configure_deploy` **默认关**；保留 `set_space_accel_pct` 供复测；护洞仍为 Soft 默认。P2c 是否开另议（本轮峰因未证明是「run 内死页」）。

## 复现

```bash
cd embdb/x && make harness-ab-space
# 或：bash scripts/harness_ab_space.sh
```
