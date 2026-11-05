# eng-debt-split backup

- stamp: `20261105-121821`
- pre-split sha256: `4b4af3ca055bfd38e604882e109e3b9e0d0f827ab749817f497d2a2cf2e30c03`
- files: `fcs.h.pre-split` (persistent) + `fcs.h.pre-split.20261105-121821`
- post-split copies: `*.post-split.20261105-121821`
- gates: `make bins` PASS · `make ci-smoke` PASS · `make test` PASS (`make_test.log`)
- pit: **169** · doc: `docs/30-discuss/ENG_DEBT_SPLIT.md`

**保留本目录**（用户要求持久备份；勿删 `fcs.h.pre-split`）。

Restore:
```sh
cp backup/eng-debt-split/fcs.h.pre-split include/fcs.h
rm -f include/fcs_codec.h include/fcs_dir.h include/fcs_sync.h include/fcs_store.h
```
