# Idle / Post 动态整理（坑 149/150/151）

> 根：写路径 `consolidate_step` = insert 税。  
> 150：Post 写中只记债；`soft_cons_repay` 写后还。  
> **151：Soft / `configure_deploy` 默认 Post**（门禁可议通过）。

## API

```cpp
s.configure_deploy();                 // Soft + Post 默认
// 写 …
s.soft_cons_repay(64);                // 读服务前（或 polish）
s.set_soft_cons(Store::SoftCons::Off); // 消融回写换读
```

## 门禁（坑 151 · `VERDICT_POST_SOFT_ON.md`）

| 项 | 结果 |
|----|------|
| 消融 200k insert Post/Soft | **0.98×** · cursors **1.30** |
| Soft A1 ×3 peak / insert | **+0.4%** / **+12.4%**（≤+5% / ≤+15%） |
| peers sync range vs SQL | base 3.97× → Post **0.46×** |
| 判决 | **升 Soft 默认：可议通过** |

复现：`bash scripts/soft_post_on_verify.sh`

## 写税消融（参考）

| arm | N | insert | cursors |
|-----|--:|-------:|--------:|
| soft Off | 200k | 6.6 | 6.5 |
| **post+repay** | 200k | **6.5** | **1.30** |
| eager | 200k | 52 | 1.16 |

## peers

`backup/peers-post/PEERS_POST.md` — sync+Post+repay：range 反超 SQLite、vs LMDB ~2.2×（坑 145 地板）、space **0.47×** LMDB。

## 判读

1. Soft 默认 Post **不**在写中 tidy；range SLA 仍须 `soft_cons_repay` / `polish`。  
2. Soft live 未 repay 时 range 仍写换读（与旧 Soft Off 同形）。  
3. Eager/Idle 仍勿升默认。
