# VERDICT_RANGE_PAGE_VIEW（坑 160）

同库交叉 · Soft Post repay 后 range · 两趟中位 on/off

| pass | on/off |
|------|-------:|
| 1 | 0.796× |
| 2 | 0.715× |
| avg | 0.756× |

## VERDICT: **KEEP Soft range_page_view**

约束：`select`/`scan` 回调内勿 `get`/`page`（钉 pin）。Soft `configure_deploy` 默认开。

复现: `bash scripts/ablate_range_page_view.sh`
