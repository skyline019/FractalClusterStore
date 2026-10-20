# XDB v2 —— 构建与测试
# 用法:
#   make            # 全部测试
#   make t3         # 单个测试
#   make bench      # 写放大基准（v2）
#   make asan       # 地址/未定义行为检查
#   make dbg        # 打开全部断言与追踪

# 测试行都是 `$(BIN)/x | tail -N`：没有 pipefail 时管道的退出码是 tail 的，失败的测试会被吞掉
#（A2 第一次抓到 g3 的泄漏时 make test 仍然 rc=0 —— 就是这么发现的）。
SHELL       := /bin/bash
.SHELLFLAGS := -o pipefail -ec

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread
INC      := -Iinclude
BIN      := build

TESTS    := t1 t2 t3 t4 t5 t6 tchk x1 x2 f1 f2 f3 f4 f5 f6 q1 p1 m1 \
            v1 g1 g2 g3 g4 g5 g6 g7 g8 g9 g10 g11 g12 g13 g14 g15 g16 g17 g18 h1 h2 h3 w1 s3 s4 c1 r2 s2 b1
BINS     := $(addprefix $(BIN)/,$(TESTS))

# 调试开关（按需组合）：
#   -DXDB_ASSERT_SORTED   内页槽序/重复子页断言
#   -DXDB_SETTLE_CHECK    settle/split 前后条目数一致性
#   -DXDB_KTRACE=<key>    指定键的构建追踪
#   -DXDB_ALLOC_TRACE     页分配/回收日志
#   -DXDB_ORPHAN_TRACE    verify 时列出孤儿页及其构建历史
#   -DXDB_RB_TRACE        回退重建链自检
#   -DXDB_CORRUPT_TRACE   CORRUPT 返回点定位
#   -DXDB_COMMIT_CHECK    每次提交后全量校验（极慢）

.PHONY: all test bins ci-smoke ci asan dbg clean
all: test

# 仅编译全部测试二进制（CI compile 门）
bins: $(BINS)

# 快速门：格式 / 近轮回归（CI smoke）
ci-smoke: $(BIN)/f1 $(BIN)/g16 $(BIN)/g17 $(BIN)/g18 $(BIN)/h2
	$(BIN)/f1
	$(BIN)/g16
	$(BIN)/g17
	$(BIN)/g18
	$(BIN)/h2

# 本地对齐严格 CI（不含 sanitizers；那些用 make asan）
ci: bins ci-smoke test

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/%: tests/%.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

test: $(BINS)
	@echo "== t1 基础"; $(BIN)/t1
	@echo "== t2 随机事务（5 个种子）"; for s in 1 2 3 4 5; do $(BIN)/t2 $$s | tail -1; done
	@echo "== t3 规模/快照/回退/恢复"; $(BIN)/t3 | tail -4
	@echo "== t4 只插入"; $(BIN)/t4 | tail -1
	@echo "== t5 边界"; $(BIN)/t5 | tail -1
	@echo "== t6 大量删除"; $(BIN)/t6 | head -2
	@echo "== tchk 树/链一致"; $(BIN)/tchk | tail -4
	@echo "== x1 语义回读"; $(BIN)/x1 | tail -2
	@echo "== x2 写路径一致性"; $(BIN)/x2 2 2 | tail -2
	@echo "== f1 FCS 语义/随机对拍"; $(BIN)/f1 | tail -1
	@echo "== f2 FCS 快照/回退"; $(BIN)/f2 | tail -1
	@echo "== f3 FCS NOR/掉电"; $(BIN)/f3 | tail -1
	@echo "== f4 FCS 规模/重开"; $(BIN)/f4 | tail -1
	@echo "== f5 FCS 空间回收/收缩"; $(BIN)/f5
	@echo "== f6 FCS 存活账精确性"; $(BIN)/f6
	@echo "== q1 FCS 查询/游标/索引"; $(BIN)/q1 | tail -1
	@echo "== p1 FCS 页内前缀压缩"; $(BIN)/p1 | tail -3
	@echo "== m1 FCS run 元数据页（命中/篡改/老格式/自愈）"; $(BIN)/m1
	@echo "== v1 FCS 溢出值（大 value / 覆盖 / 压实 / 回收）"; $(BIN)/v1 | tail -3
	@echo "== g1 FCS 范围墓碑"; $(BIN)/g1 | tail -2
	@echo "== w1 FCS 多写者 + 自动组提交"; $(BIN)/w1
	@echo "== s3 FCS 快照期间回收不停摆"; $(BIN)/s3 | tail -3
	@echo "== g2 FCS 大量范围墓碑 + 存活账"; $(BIN)/g2 | tail -3
	@echo "== s4 FCS 多版本快照 + 快照感知页复用"; $(BIN)/s4 | tail -4
	@echo "== g3 FCS 同 lo 墓碑 / 长删循环退休 / 未提交大值 / 链缓存失效"; $(BIN)/g3 | tail -5
	@echo "== g4 FCS 异步写×快照回退（预取在飞页 / pend_delta_ 顺序 / 快照含逻辑提交）/ 压实布局"; $(BIN)/g4 | tail -5
	@echo "== g5 FCS 交错模糊（put/del/del_range/select × commit(false)/snapshot/rollback/维护/scrub/reopen，4 种子）"; $(BIN)/g5 | tail -5
	@echo "== g6 FCS 掉电注入（易失写缓存 + 随机掉电/撕裂 ⇒ 重开 == 某个提交点 + verify/scrub，4 种子）"; $(BIN)/g6 | tail -5
	@echo "== g7 FCS 读写并发（1 写者 op 流 × 2 读者 get/select 不变量校验，2 种子，共享读模式）"; $(BIN)/g7 | tail -3
	@echo "== g7 独占读模式（set_shared_reads(false)）+ 4 读者共享（坑 86 写者饥饿回归）"; G7_EXCL=1 $(BIN)/g7 1 3000 | tail -1; $(BIN)/g7 3 4000 4 | tail -1
	@echo "== g7 40 读者（> 32 线程槽：槽溢出走独占路径，H5）"; $(BIN)/g7 5 1500 40 | tail -1
	@echo "== g8 FCS 覆盖风暴 × 多单元 × 层合并 × 快照钉住（每次提交全量比对；坑 89/90/91 回归，3 组）"; $(BIN)/g8 1 4000 160 200 | tail -1; $(BIN)/g8 2 2000 200 150 | tail -1; $(BIN)/g8 3 8000 120 400 | tail -1
	@echo "== g9 FCS 墓碑账目口径（坑 92/93/94/95：快照期同 lo 墓碑 / 悬空点墓碑退休 / 底层点墓碑丢弃 / 整盖快路径）+ 风暴 6 组（每提交全量比对含快照视图）"; $(BIN)/g9 | tail -7
	@echo "== g10 FCS R2 层合并预算 + 分步维护（consolidate_step / compact_file_step：每步 verify + 快照视图抽查）+ R7 墓碑自动归一 + R8 局部 TRIM"; $(BIN)/g10 | tail -6
	@echo "== g11 FCS 第 12 轮：提交后维护（R3-lite）+ 分步层合并作业（R2b）+ 坑 99 SB 闸/NOSPACE 重试 + 并发读者"; $(BIN)/g11 | tail -5
	@echo "== g12 FCS 坑 102：逻辑提交版本回绕（70000 次逻辑提交 + 跨回绕持有快照 ⇒ 视图 / verify / 重开 / 回退）"; $(BIN)/g12 | tail -1
	@echo "== g13 FCS 第 13 轮：流式归并产物 + 64 位 seq/ver 与 v1 清单链升级（含掉电，第 16 轮改写）+ 层合并新规则 (c)(d)(e) + 维护预算 / mode 3 后台线程"; $(BIN)/g13 | tail -10
	@echo "== g14 FCS 第 14 轮：作业收尾单元级增量账（账零漂移）+ 文件高水位（预留按存活估 / 就地续租 / 顶部回落 / hw 回收策略）+ mode 3 读者让路"; $(BIN)/g14 | tail -9
	@echo "== g15 FCS 第 15 轮：高水位回收二步搬家（规划器 / 一提交点一条 / 窗外落地 / 快照钉住是墙）"; $(BIN)/g15 | tail -4
	@echo "== g16 FCS 第 16 轮：P2b 分片归并 + RUN_TRIM（分片基本面 / 跨重开重放 TRIM / abort+rollback / 快照作废 / 溢出链 / 墓碑不复活）"; $(BIN)/g16 | tail -7
	@echo "== g17 FCS 第 17 轮：fsync 出锁（提交分段 / 提交顺序器 / 两个 SB 代数 / durable_vers；慢 sync 设备下读者不被挡、空票等持久、阶段二失败粘性、在飞时快照、3 写者×mode 3）+ 家族虚拟源游标（源 >64）+ 中等产物按洞分片"; $(BIN)/g17 | tail -9
	@echo "== g18 FCS 护洞 + 整理态 + 碎片切片 A + 有界并洞 B"; $(BIN)/g18 | tail -6
	@echo "== h1 挂载模糊（合法镜像随机翻字节 ⇒ open 只许 OK/CORRUPT/IO，坏数据必须查得出来，不崩不挂）"; $(BIN)/h1 400 1 | tail -2; $(BIN)/h1 400 2 | tail -1
	@echo "== h2 golden 镜像兼容（tests/golden/*.img v2：挂载 + 逐键比对 + 快照视图 + verify；tests/golden/v1/*.img 老库：只读不写回 → 写即升级 → 重开）"; $(BIN)/h2 | tail -7
	@echo "== h3 多写者 × 异步提交 × 掉电（3 写者线程 + VolDev 随机掉电/撕裂，每线程自己的持久下界模型，3 种子 × (10 轮长 span + 40 轮短 span)）"; $(BIN)/h3 | tail -7
	@echo "== c1 FCS scrub（介质 CRC）"; $(BIN)/c1 | tail -3
	@echo "== r2 FCS 空间耗尽/写失败/不挂死"; $(BIN)/r2
	@echo "== s2 FCS 长跑无漂移"; $(BIN)/s2 | tail -4
	@echo "== b1 FCS 大库（>256 MB，PageId 32 位）"; $(BIN)/b1 | tail -3

# 基准：v3（FCS）、v2（默认缓存）、v2（大缓存）、v1（对照）、sqlite 对比
bench: | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) bench/bench3.cpp -o $(BIN)/bench3r
	$(CXX) $(CXXFLAGS) $(INC) bench/bench4.cpp -o $(BIN)/bench4 -lsqlite3
	@echo "# v3 矩阵"
	@$(BIN)/bench3r 20000 500 | tail -2
	@$(BIN)/bench3r 50000 1000 | tail -2
	@$(BIN)/bench3r 200000 2000 | tail -2
	@echo "# 组提交（每 8 个逻辑提交落盘一次）"
	@$(BIN)/bench3r 20000 100 8 | tail -3
	@$(BIN)/bench3r 200000 2000 8 | tail -3
	$(CXX) $(CXXFLAGS) $(INC) bench/bench5.cpp -o $(BIN)/bench5
	@echo "--- bench5：A2 大值 / B2 预读 / B3 多写者"
	@$(BIN)/bench5 big 3001 4000
	@$(BIN)/bench5 big 8160 2000
	@$(BIN)/bench5 big 8192 2000
	@$(BIN)/bench5 scan 128 20000 1
	@$(BIN)/bench5 scan 128 20000 0
	@$(BIN)/bench5 mt 1 20000
	@$(BIN)/bench5 mt 4 5000
	$(CXX) $(CXXFLAGS) $(INC) bench/bench6.cpp -o $(BIN)/bench6
	@echo "--- bench6：读并发（共享读 / 独占）/ 大值重复读 / 长删循环 / compact_file 空间曲线 / 快照期维护"
	@$(BIN)/bench6 rget 1 100000
	@$(BIN)/bench6 rget 2 100000
	@$(BIN)/bench6 rget 4 100000
	@$(BIN)/bench6 rget 2 100000 4096 0
	@$(BIN)/bench6 snapmaint 2000 300 1
	@$(BIN)/bench6 bigget 16 20000
	@$(BIN)/bench6 bigget 64 20000
	@$(BIN)/bench6 delloop 4 100000
	@$(BIN)/bench6 cfcurve 20000 3900 3
	$(CXX) $(CXXFLAGS) $(INC) bench/bench2.cpp -o $(BIN)/bench2
	$(CXX) $(CXXFLAGS) $(INC) -DXDB_NAV_CAP=512 bench/bench2.cpp -o $(BIN)/bench2big
	$(CXX) $(CXXFLAGS) -I../include ../bench/bench_v1.cpp ../src/engine.cpp -o $(BIN)/bench1
	@echo "# engine,ops,commit_every,value_bytes,write_bytes,read_bytes,writes,reads,seconds"
	@$(BIN)/bench1 20000 500 | head -1
	@$(BIN)/bench2 20000 500 | head -1
	@$(BIN)/bench2big 20000 500 | head -1

asan:
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/f1.cpp -o $(BIN)/asan_f1
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_f1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/f4.cpp -o $(BIN)/asan_f4
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_f4 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/q1.cpp -o $(BIN)/asan_q1
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_q1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/f5.cpp -o $(BIN)/asan_f5
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/f6.cpp -o $(BIN)/asan_f6
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_f5 | tail -1
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_f6 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/t3.cpp -o $(BIN)/asan_t3
	$(BIN)/asan_t3
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/t2.cpp -o $(BIN)/asan_t2
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_t2 2
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/m1.cpp -o $(BIN)/asan_m1
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_m1 | tail -1
	for t in v1 g1 g2 g3 g4 g5 g6 g7 g8 g9 g10 g11 g12 g13 g14 g15 g16 g17 g18 h1 h2 h3 s3 s4 c1; do \
	  $(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/$$t.cpp -o $(BIN)/asan_$$t; \
	  ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_$$t | tail -1; \
	done
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/w1.cpp -o $(BIN)/asan_w1
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_w1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/w1.cpp -o $(BIN)/tsan_w1
	$(BIN)/tsan_w1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/g7.cpp -o $(BIN)/tsan_g7
	$(BIN)/tsan_g7 2>&1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/g11.cpp -o $(BIN)/tsan_g11
	$(BIN)/tsan_g11 2>&1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/g13.cpp -o $(BIN)/tsan_g13
	$(BIN)/tsan_g13 mode3 2>&1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/g17.cpp -o $(BIN)/tsan_g17
	$(BIN)/tsan_g17 60 2>&1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=thread $(INC) tests/g14.cpp -o $(BIN)/tsan_g14
	$(BIN)/tsan_g14 yield 2>&1 | tail -1
	$(BIN)/tsan_g7 3 3000 4 2>&1 | tail -1
	G7_EXCL=1 $(BIN)/tsan_g7 1 2000 2>&1 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/r2.cpp -o $(BIN)/asan_r2
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_r2 | tail -1
	$(CXX) -std=c++17 -O1 -g -pthread -fsanitize=address,undefined $(INC) tests/s2.cpp -o $(BIN)/asan_s2
	ASAN_OPTIONS=detect_leaks=1 $(BIN)/asan_s2 | tail -1

dbg: | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) -DXDB_ASSERT_SORTED -DXDB_SETTLE_CHECK -DXDB_CORRUPT_TRACE tests/t3.cpp -o $(BIN)/t3dbg
	$(BIN)/t3dbg

clean:
	rm -rf $(BIN)
