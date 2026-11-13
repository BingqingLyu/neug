# Module 3: 四结构协同 remap

**Goal**: 按同一 old_vid→new_vid 映射同步重排四结构（属性列、vid/indexer+时间戳、CSR、索引），保证按新 vid 一致（FR-010~014）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T301] LFIndexer keys 重编 + indices 全局重建

**description**: LFIndexer 加 remap：keys 按映射重排（组内 gather）、indices 按全部有效 vid 全局重建哈希槽（FR-010、FR-011）。

**details**:
- include/neug/utils/id_indexer.h（+ 实现）加 remap 接口。
- keys_[vid]=oid 按映射重排（组内 gather）。
- indices_[hash(oid)]→vid 全局重建：扫所有有效 vid 取 oid，重新插入哈希槽。MUST NOT 原地逐槽更新（开放寻址删除会断探测链）。
- 重建后所有有效 PK 的 oid→vid 查找正确。
- 验收：remap 后 PK 查找正确；indices 重建成本正比有效顶点总数（记录为代价）。

## [F013-T302] VertexTimestamp 组段重编

**description**: VertexTimestamp 按映射重排 inserted_vertices_（组段局部），removed_vertices_ 相应更新（FR-010）。

**details**:
- include/neug/storages/graph/vertex_timestamp.h + src/storages/graph/vertex_timestamp.cc 加组段重编接口。
- inserted_vertices_[vid-init_vertex_num_] 按映射重排；removed_vertices_ 稀疏集相应更新。
- 内存原地共享 CAS 语义不变（重编在排他窗口内执行）。
- 验收：重排后可见性判定按新 vid 正确；删除行不再可见。

## [F013-T303] IndexIDAccessor 线性 remap

**description**: vid_to_index_id_ 按映射线性 remap，index_id_to_vid_ 反向 map 重建；index_id 与引擎 blob 不变（FR-010、FR-012、FR-014）。

**details**:
- include/neug/storages/index/index_id_accessor.h + src/storages/index/index_id_accessor.cc 加 remap 接口。
- vid_to_index_id_ 按映射重排（组内局部）；index_id_to_vid_ 重建（vid 值变）。
- index_id 不变；引擎 blob 按 index_id 寻址不动；MUST NOT 回收 index_id tombstone（与 vid 重排解耦）。
- 验收：remap 后 vid→index_id 映射正确；引擎 blob 未变。

## [F013-T304] M3 四结构 remap 一致性测试

**description**: 覆盖四结构 remap 一致性、indices 重建后 PK 查找、accessor remap、删除行回收、引擎 blob 不动（FR-010~014）。

**details**:
- tests/storage/test_row_group_compaction.cc：重排后属性/PK 查找/可见性/索引逐行一致；删除行回收；引擎 blob 不动。
- 验收：FR-010~014 可验证。