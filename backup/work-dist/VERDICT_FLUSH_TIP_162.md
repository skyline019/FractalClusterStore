# 坑 162 → 163（flush_tip 漂移）

## 162

S9：`tip.rid==NIL && prev && !full_probe` 跳过 tip-miss probe ⇒ 更新漏死账。`f1` 复现。

## 163（最终）

162 修 tip 全量写后 **g16 仍漂**：经典路径信 `DualTip.osz`，RUN_TRIM/分片后陈旧 tip 双计 dead。

**修法**：经典 `flush_tip` 只用 bloom 跳过首写 settle；更新一律 `probe_prev`；TRIM bump `dual_tip_gen_`。

**判决**：`f1`/`g16`/`ci-smoke` 绿；`ablate_flush_tip` insert **0.83×** ⇒ **KEEP 默认 on**。  
完整 peers：`backup/peers-full/PEERS_FULL.md`。
