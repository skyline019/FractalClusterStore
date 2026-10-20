# 注释模块化精修前备份（2026-10-21）

路径：`embdb/x/backup/r18/pre-comment-modular/`

含：`fcs.h` `common.h` `blockdev.h` `layout.h` `store.h`

## 还原

```sh
cd embdb/x
cp -a backup/r18/pre-comment-modular/fcs.h include/fcs.h
cp -a backup/r18/pre-comment-modular/common.h include/common.h
cp -a backup/r18/pre-comment-modular/blockdev.h include/blockdev.h
cp -a backup/r18/pre-comment-modular/layout.h include/layout.h
cp -a backup/r18/pre-comment-modular/store.h include/store.h
# 建议：make -s build/g18 && ./build/g18
```
