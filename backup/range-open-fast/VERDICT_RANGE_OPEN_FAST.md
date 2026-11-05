# VERDICT_RANGE_OPEN_FAST（坑 158）

Soft sync Post 50k · repay 后 range。

## 根因（探针）

| 观察 | 值 |
|------|----|
| rows/scan（lo+4096） | **≈1.0** |
| open 占 select 墙钟 | **~83%** |
| 对手 LMDB | 复用游标 + mmap SET_RANGE |

leading→verified：peers range 慢头在 **每 select 的 open/prime**，非 k-way / next。

## 刀

- 空 mem：`refresh_mem` 跳过 `mem_sorted`
- 单元相交段 =1：`try_prime_oneseg_` 跳过 cands/`build_chains`
- 消融：`set_range_open_fast(false)`

## 同库交叉消融（只测 range · 5 轮中位 · 两趟）

| 趟 | on | off | on/off |
|----|---:|----:|-------:|
| 1 | 0.636 | 0.726 | **0.88×** |
| 2 | 0.657 | 0.732 | **0.90×** |

全画像 5 轮写路径噪声大（get/update 不应被本刀触达）；**以同库 xab 为准**。

## VERDICT: **KEEP** `range_open_fast_=true` 默认

复现：
```bash
bash scripts/ablate_range_open_fast.sh
g++ -O2 -std=c++17 -I. backup/range-open-fast/probe_xab.cpp -o /tmp/xab -lpthread && /tmp/xab
```
