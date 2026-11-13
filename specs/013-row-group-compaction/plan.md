# Implementation Plan: 行组逻辑压实（组内 vid 重排与空洞回收）

**Branch**: `013-row-group-compaction` | **Date**: 2026-11-13 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/013-row-group-compaction/spec.md`

## Summary

在 012 row group 结构地基（布局 B、`base_g=g×G` 均匀纯算术、组枚举、按组脏统计）之上，实现**组内 vid 重排（逻辑压实）**：
按空洞率选脏组 → 生成 old_vid→new_vid 紧凑映射 → 四结构（属性列、vid/indexer+时间戳、CSR、索引）协同 remap →
组尾留洞 → 每组前沿回填复用。删除行不被携带、其字节随旧数据经 GC 回收（空间收益）。全程在既有全局排他维护窗口
（`WithCheckpointMaintenance`）内执行、checkpoint 单点原子发布，compaction 不做第二发布者。

**关键边界**：仅逻辑压实，**解耦四结构分块化**（remap 是 vid 引用的逻辑重写，整块持久化即可）；物理压实
（base+delta 按组 merge）另立后续 spec。不改组大小、不做物理分段、不动 `pages_[vid>>page_shift_]` 读路径。

## Technical Context

**Language/Version**: C++20（clang-format style=file）
**Primary Dependencies**: 012 row group 地基（`RowGroupLayout`/`RowGroup`/`Table::group_*`）；现有存储栈
`LFIndexer`（id_indexer.h）、`VertexTimestamp`（vertex_timestamp.h）、`IndexIDAccessor`（index_id_accessor.h）、
`MutableCsr`/`ImmutableCsr`（csr/）、`ChunkedColumn`；`GraphSnapshotStore::WithCheckpointMaintenance` 排他窗口；
admin 命令路径（`AdminType`，execution_slot.cc）。无新增第三方依赖。
**Storage**: 整块持久化（indexer keys/indices、时间戳、CSR 整文件、accessor）；属性列 chunk 目录（012 v1 flags）。
remap 产物经 checkpoint manifest 原子发布。
**Testing**: GTest（tests/storage、tests/transaction）+ TSAN 并发压测 + 磁盘故障注入 harness（test_chunk_fault_recovery）；
性能评测 vs main 双构建 A/B（evaluation 阶段，优先级低于实现）。

**已核实的关键事实**（计划依据）：
- vid 分配：`insert_vertex_pk_internal`（vertex_table.cc L188）新 PK 走 `indexer.insert` append-only；同 PK 重插复用原 vid。
- `LFIndexer`：`keys_[vid]=oid`（ColumnBase）+ `indices_[hash(oid)]→vid`（TypedColumn，开放寻址）+ `num_elements_`；**无 remap 接口**。
- `VertexTimestamp`：`inserted_vertices_[vid-init_vertex_num_]` atomic ts 数组 + `removed_vertices_` 稀疏集；`Compact()` 仅压时间戳。
- `IndexIDAccessor`：`vid_to_index_id_`（IDataContainer 数组）+ `index_id_to_vid_`（unordered_map）+ `next_index_id_`（单调）；
  remap 只动 vid 侧，index_id 与引擎 blob 不变。
- CSR：`adj_list_buffer_[vid]`→`nbr_t{neighbor, atomic timestamp, data}`；`compact()` 清删除边、不改 vid；**无 neighbor remap 接口**。
- 排他窗口：`WithCheckpointMaintenance`（graph_snapshot_store.cc L242）drain readers + update exclusive；checkpoint 单点发布。
- admin 命令：`AdminType::kCheckpoint`（execution_slot.cc L296）；COMPACT 类比新增 `AdminType::kCompact`。

## Project Structure

```text
include/neug/storages/graph/
├── row_group_compaction.h        # [新] 压实编排器：候选选择 + 映射生成 + 四结构 remap 协调 + 窗口发布
├── vertex_table.h                # [改] 组重排入口、每组前沿成员、空洞统计
├── edge_table.h                  # [改] CSR neighbor remap 入口
include/neug/utils/
├── id_indexer.h                  # [改] LFIndexer remap：keys gather + indices 全局重建
├── property/
│   ├── chunked_column.h          # [改] 组内行 gather（按映射重排有效行）
│   └── group_frontier.h          # [新] 每组追加前沿分配器（vid=base_g+frontier_g）
include/neug/storages/
├── graph/vertex_timestamp.h      # [改] 组段重编（按映射重排 inserted_vertices_）
├── index/index_id_accessor.h     # [改] vid_to_index_id_ 线性 remap + 反向 map 重建
├── csr/mutable_csr.h             # [改] neighbor remap（改写 nbr_t.neighbor）
├── csr/immutable_csr.h           # [改] neighbor remap（同上）
├── graph/graph_snapshot_store.h  # [改] 维护窗口暴露压实编排入口
src/storages/graph/
├── row_group_compaction.cc       # [新] 编排器实现
├── vertex_table.cc               # [改] 组重排 + 前沿分配接入 insert_vertex_pk_internal
├── edge_table.cc                 # [改] CSR remap 实现
src/utils/id_indexer.cc           # [改] remap 实现（或头文件内联）
src/storages/graph/vertex_timestamp.cc  # [改] 组段重编
src/storages/index/index_id_accessor.cc # [改] remap 实现
src/storages/csr/mutable_csr.cc   # [改] neighbor remap
src/main/
├── execution_slot.cc             # [改] AdminType::kCompact 路由
├── checkpoint_coordinator.cc     # [改] 维护窗口内触发压实
src/compiler/...                  # [改] COMPACT 命令解析（类比 CHECKPOINT）
include/neug/config.h.in          # [改] 空洞率阈值配置项
tests/storage/
├── test_row_group_compaction.cc  # [新] 重排/remap/回填/崩溃单测与集成
tests/transaction/
└── test_concurrent_compaction.cc # [新] 并发 insert×重排 TSAN 压测
```

**Structure Decision**: 新增 2 个头文件（编排器 + 每组前沿）+ 对应实现；改动集中在 storages 层（四结构 remap）+
main 层（命令路由 + 窗口触发）+ compiler（COMPACT 解析）。复用 012 组枚举与既有 admin/维护窗口路径，最小化新增面。
**不新建 CMake 目标**（测试并入现有 storage/transaction 测试注册）。

## Data Model

本特性的数据模型：
1. 每组空洞统计（GroupHoleStats）
2. 每组追加前沿（GroupFrontier）
3. vid 重排映射（VidReorderMap）
4. 压实候选集（CompactionCandidates）
5. 空洞率阈值配置

### 1. 每组空洞统计（GroupHoleStats）

**Data Structure**: 每组的有效行数与空洞率，候选选择的输入。

```
GroupHoleStats { group_id, valid_count, hole_rate }
hole_rate = 1 - valid_count / group_capacity
```

例：G=8、组 0 有效 6 行 → hole_rate=0.25。

**Data Access & Update**:
- **计算**: 扫该组时间戳（`VertexTimestamp::IsVertexValid`）统计有效行；按需计算，不常驻热路径。
- **更新**: delete 后空洞率升；重排后有效行紧凑、空洞率重算。
- **持久化**: 可由时间戳重建，跨 checkpoint/重开保留（FR-004）。

### 2. 每组追加前沿（GroupFrontier）

**Data Structure**: 每组的下一个可分配 vid 偏移，替代全局计数器。

```
GroupFrontier { frontier_g }   // vid = base_g + frontier_g
```

例：组 0 压实后有效 3 行 → frontier_0=3；下次 insert vid=base_0+3（回填组尾洞）。

**Data Access & Update**:
- **分配**: insert 取 base_g+frontier_g，frontier_g++（并发原子递增）。
- **回退**: 压实后 frontier_g = valid_count。
- **初始**: 稠密态 frontier_g=组内行数，顺序 append 活跃组尾部（与基线全局计数器一致，FR-019）。
- **持久化**: frontier 随 checkpoint 落盘（manifest 标量或组元数据），重开恢复。

### 3. vid 重排映射（VidReorderMap）

**Data Structure**: 单次重排的 old_vid→new_vid 紧凑映射，仅覆盖有效 vid。

```
VidReorderMap { old_vid -> new_vid }   // 删除行不在映射中
```

例：组 0 [v0, v1(删), v2, v3] → {v0→v0, v2→v1, v3→v2}；v1 无映射（删除行被丢弃）。

**Data Access & Update**:
- **生成**: 扫组内有效 vid 按原顺序紧凑编号（new_vid=base_g+新偏移）。
- **消费**: 四结构 remap 共用同一映射。
- **生命周期**: 重排过程临时量，不落盘（落盘的是 remap 后的新布局）。

### 4. 压实候选集（CompactionCandidates）

**Data Structure**: 待重排的组集合 + 触发模式。

```
CompactionCandidates { groups[], mode(auto|manual), scope(all|label) }
```

**Data Access & Update**:
- **自动**: 后台调度选 hole_rate≥阈值的组。
- **手动**: `COMPACT [TABLE label]` 选 scope 内 hole_rate>0 的组（不过阈值）。
- **排序**: 按邻居波及面（度数）升序，优先低波及组。
- **持久化**: 跨 checkpoint/重开保留（可由空洞统计重建）。

### 5. 空洞率阈值配置

**Data Structure**: `compaction_hole_rate_threshold`（config.h.in，默认保守如 0.3）。
**Data Access & Update**: 用户可配置；仅自动模式消费；手动命令绕过。

## Algorithm Model

本特性的算法模型：
1. 空洞率计算与候选选择（M1）
2. 组内重排映射生成（M2）
3. 四结构协同 remap（M3）
4. CSR 邻居集合定位与 remap（M4）
5. 组尾回填与前沿分配（M5）
6. 排他窗口编排与原子发布（M6）

### 1. 空洞率计算与候选选择

**Algorithm Target**: 选出值得重排的脏组（自动按阈值、手动按命令）。

**Algorithm Details**:
- 对每组扫时间戳统计 valid_count，hole_rate=1−valid_count/G。
- 自动：hole_rate≥阈值入选；手动：scope 内 hole_rate>0 入选（不过阈值）。
- 估计每组邻居波及面（out/in 度数之和），候选按波及面升序（优先低波及）。
- 批量预算：一次窗口最多重排 N 组（可配）。
- 例：G=8，组 0 有效 6（hole 0.25）、组 1 有效 2（hole 0.75），阈值 0.3 → 自动只选组 1；手动 COMPACT 选组 0+组 1。

### 2. 组内重排映射生成

**Algorithm Target**: 生成 old_vid→new_vid 紧凑映射，有效行 gather 到组前部、组尾留洞、base_g 不变。

**Algorithm Details**:
- 扫组内 vid（base_g..base_g+G），按原顺序取有效 vid，new_vid=base_g+递增偏移。
- 删除行不进映射（不被携带，字节随旧数据 GC 回收）。
- 例：组 0 [v0,v1(删),v2,v3] → {v0→0, v2→1, v3→2}（相对 base_0），组尾 [3,8) 留洞，base_0 不变。

### 3. 四结构协同 remap

**Algorithm Target**: 按同一映射同步重排四结构，保证按新 vid 一致。

**Algorithm Details**:
- **属性列**: 按映射把有效行 gather 到新偏移（组内局部）；删除行不搬。ChunkedColumn 加组内 gather 能力。
- **indexer keys**: keys_[vid]=oid 按映射重排（组内 gather）。
- **indexer indices**: 全局重建——扫所有有效 vid 取 oid，重新插入 hash(oid)→new_vid 哈希槽（开放寻址，
  原地逐槽更新会断探测链，故全量重建）。成本正比有效顶点总数（唯一不可局部化的 indexer 成本）。
- **时间戳**: inserted_vertices_ 按映射重排（组段局部）；removed_vertices_ 相应更新。
- **索引 accessor**: vid_to_index_id_ 按映射重排（组内局部）；index_id_to_vid_ 反向 map 重建（vid 值变）；
  index_id 不变、引擎 blob 不动。
- 例：v2→v1 后，keys_[1]=oid_v2、indices[hash(oid_v2)]=1、inserted[1]=ts_v2、accessor[1]=iid_v2。

### 4. CSR 邻居集合定位与 remap

**Algorithm Target**: remap 指向重排组顶点的 neighbor 字段，波及面限于组邻居集合，批量去重。

**Algorithm Details**:
- **定位**: 经重排组 g 的 out_csr/in_csr 列出 neighbor∈g 的对端（g 的 in/out 邻居），波及面被 g 度数封顶，非全图扫描。
- **remap**: 对每个邻居顶点的邻接表，扫 nbr_t，neighbor∈g 的改为映射后 new_vid。
- **批量去重**: 多组重排合并邻居集合，同一邻居邻接表只改写一次。
- **MVCC**: 窗口内已回收旧快照；remap 产生新邻接表版本（COW 单顶点粒度），发布后旧版本随 GC 回收。
- 例：组 0 重排 v2→v1，组 1 的 out_csr[4] 里 neighbor=v2 改为 v1；非邻居组不动。

### 5. 组尾回填与前沿分配

**Algorithm Target**: insert 从每组前沿分配 vid，回填组尾洞；异 PK 复用解绑旧 oid。

**Algorithm Details**:
- **分配**: vid=base_g+frontier_g，frontier_g++（并发原子）。
- **回退**: 压实后 frontier_g=valid_count。
- **异 PK 解绑**: 复用 vid 若残留旧 oid 绑定，先从 indices 移除旧 oid→vid，再插新 oid→vid（FR-021）。
- **同 PK 重插**: 复用原 vid（基线行为，不变）。
- **单调**: 组内偏移连续顺序回填，不抢压实前散布异 PK 空槽。
- 例：组 0 压实后 frontier=3，insert PK=B → vid=base_0+3；若该 vid 残留 oid=A 绑定，先解绑 A→vid。

### 6. 排他窗口编排与原子发布

**Algorithm Target**: 重排在排他窗口内回收旧快照后执行，checkpoint 单点原子发布，崩溃回退。

**Algorithm Details**:
- **窗口**: 复用 WithCheckpointMaintenance（drain readers + update exclusive），窗口内回收旧快照。
- **编排**: 窗口内批量重排候选组（映射生成→四结构 remap→CSR remap），合并去重邻居。
- **发布**: remap 产物挂 checkpoint 单点发布（原子切 manifest + reopen）；compaction 不做第二发布者。
- **最老快照封顶**: 长事务 pin 旧快照使重排无净收益时跳过/延后（不硬等）。
- **崩溃恢复**: 发布前崩溃回退重排前状态，孤儿产物经整对象 GC 清理。
- **时间线重置解耦**: 可搭同一窗口，触发/内容互不绑定。
- 例：前台写 + 周期 checkpoint，后台触发 COMPACT → 下次 checkpoint 维护窗口内批量重排 → 原子发布 → 前台窗口外不阻塞。

## Verification / Test Plan

- **Unit Tests**（test_row_group_compaction.cc）: 空洞率计算；映射生成（全有效/全删除/混合）；四结构 remap 一致性；
  indices 重建后 PK 查找；CSR 邻居 remap（跨组/跨 label/批量去重）；前沿分配/回退/异 PK 解绑；阈值选择 vs 手动命令。
- **Integration Tests**: 重排后 PK/可见性/索引/CSR 遍历端到端一致；删除行回收（ckpt_bytes 停增）；干净组隔离；
  回填闭环（重排→insert→再重排）；COMPACT 命令端到端（admin 路径）；崩溃恢复（故障注入 harness）。
- **并发压测**（test_concurrent_compaction.cc，TSAN）: 前台写 + 周期 checkpoint × 重排零 race；读者持旧快照×重排隔离；
  并发 insert 不产生重复 vid/PK 冲突。
- **性能评测**（evaluation 阶段，优先级低于实现）: vs main 双构建 A/B；未触发压实常规负载吞吐/延迟不回退
  （重点每组前沿 insert 路径）；触发压实空间回收 vs remap 代价 + 窗口停顿；参考「分块 COW 基准评测计划.md」指标维度
  与 workload 轴，扩展异 PK delete churn 与局部重排口径（W14）。

**构建验证**:
```bash
cmake --build build -j$(sysctl -n hw.physicalcpu) --target chunk_foundation_test test_row_group_compaction
GLOG_minloglevel=3 ./build/tests/storage/test_row_group_compaction
cmake --build build -j$(sysctl -n hw.physicalcpu)   # 全量
make format-check
```

## Delivery / Git

- 分支 `013-row-group-compaction`（已从 012-row-group-view b18c6908 切出）。
- **选择性提交**：仅本计划触碰的文件 + spec/plan/tasks；工作区无关改动（PK 预占、mvcc_insert、submodule 脏指针、
  未跟踪文档）一律不进提交；`git add` 逐文件 + `git diff --cached` 核对。
- 提交风格 `feat(storage): ...`；推送 origin（BingqingLyu fork）需另行征得同意；不混进 #1083。
- 审阅方式：默认 workspace 本地审阅，推 GitHub 需另行征得同意。

## Assumptions & Boundaries

- 仅逻辑压实，解耦四结构分块化（remap 整块重写即可）；物理压实（base+delta 按组 merge）另立后续 spec。
- 固定组大小、base_g=g×G 均匀；不改组大小、不做物理分段、不动读路径。
- indices 全局重建成本正比有效顶点总数，是组内重排唯一不可局部化的 indexer 成本；大表重排须计入。
- vid/gid 不承诺跨重排稳定（R15），稳定标识用 PRIMARY KEY；索引 tombstone 回收与时间线重置均解耦（R14/R16）。
- 变长属性空洞回收依赖 010，本特性不处理；bundled 边跟随 CSR。
- 性能基准属 implementation 后 evaluation，优先级低于实现；正确性验证（TSAN/崩溃/并发）随实现同步落地。

## 模块 ↔ FR 覆盖

| 模块 | FR | 算法 |
| --- | --- | --- |
| M1 空洞统计与候选选择 | FR-001~005 | 算法 1 |
| M2 组内重排映射与 gather | FR-006~009 | 算法 2 |
| M3 四结构协同 remap | FR-010~014 | 算法 3 |
| M4 CSR 邻居跨组 remap | FR-015~018 | 算法 4 |
| M5 组尾回填与前沿分配 | FR-019~023 | 算法 5 |
| M6 排他窗口编排与原子发布 | FR-024~028 | 算法 6 |