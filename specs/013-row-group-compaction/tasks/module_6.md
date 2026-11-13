# Module 6: 排他窗口编排与原子发布

**Goal**: vid 重排在既有全局排他窗口内回收旧快照后执行，批量重排摊薄静默成本，checkpoint 单点原子发布，崩溃回退（FR-024~028）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T601] row_group_compaction 编排器

**description**: 新增压实编排器，协调候选选择→映射生成→四结构 remap→CSR remap→前沿回退（FR-024 协调）。

**details**:
- 新建 include/neug/storages/graph/row_group_compaction.h + src/storages/graph/row_group_compaction.cc。
- 编排：候选组（M1）→ 映射生成（M2 T201）→ 属性列 gather（M2 T202）+ indexer remap（M3 T301）+
  时间戳重编（M3 T302）+ accessor remap（M3 T303）→ CSR 邻居 remap（M4）→ 前沿回退（M5）。
- 批量重排多个脏组，合并去重 CSR 邻居集合。
- 验收：编排器协调四结构 remap 一致；批量重排正确。

## [F013-T602] 排他窗口集成与批量重排

**description**: 复用 WithCheckpointMaintenance 全局排他窗口，窗口内回收旧快照后批量重排；最老快照封顶（FR-024、FR-026）。

**details**:
- src/main/checkpoint_coordinator.cc + graph_snapshot_store：维护窗口内触发压实编排器。
- 窗口内 drain readers + update exclusive，回收旧快照后才执行 remap（删除行不被携带的安全前提）。
- 最老 live 快照年龄作为一等输入；长事务 pin 使重排无净收益时跳过/延后（不硬等）。
- 与时间线重置解耦（可搭同一窗口，触发/内容互不绑定，FR-028）。
- 验收：重排在排他窗口内执行；前台读写窗口外不阻塞；长事务 pin 下跳过。

## [F013-T603] 原子发布与崩溃恢复

**description**: remap 产物挂 checkpoint 单点原子发布（manifest + reopen）；发布前崩溃回退、孤儿 GC（FR-025、FR-027）。

**details**:
- remap 产物经 checkpoint 单点发布（原子切 manifest + reopen）；compaction MUST NOT 成为第二发布者。
- 发布前崩溃回退到重排前一致状态；孤儿产物经整对象 GC 清理。
- 复用磁盘故障注入 harness（test_chunk_fault_recovery）验证多注入点。
- 验收：发布原子；任意注入点崩溃回退一致、孤儿被 GC 清理。

## [F013-T604] M6 并发与崩溃恢复测试

**description**: 覆盖前台写×重排并发 TSAN 零 race、读者持旧快照隔离、长事务跳过、崩溃恢复（FR-024~028、SC-006~008）。

**details**:
- tests/transaction/test_concurrent_compaction.cc（TSAN）：前台持续写 + 周期 checkpoint × 重排零 race；
  读者持旧快照×重排隔离；并发 insert 不产生重复 vid/PK 冲突。
- 崩溃恢复：故障注入 harness 多注入点回退一致。
- 验收：FR-024~028 可验证；TSAN 零 race（重排新代码路径）。