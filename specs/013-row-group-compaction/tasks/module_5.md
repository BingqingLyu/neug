# Module 5: 组尾空洞回填与每组前沿分配

**Goal**: 把 vid 分配从全局计数器改为每组追加前沿（vid=base_g+frontier_g），压实后前沿回退、insert 顺序回填组尾洞；异 PK 复用解绑旧 oid（FR-019~023）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T501] GroupFrontier 每组前沿分配器

**description**: 新增每组追加前沿分配器，vid=base_g+frontier_g；初始稠密态与基线全局计数器一致（FR-019）。

**details**:
- 新建 include/neug/utils/property/group_frontier.h（+ 实现）：每组维护 frontier_g。
- 分配 vid=base_g+frontier_g，frontier_g++（并发原子递增）。
- 初始稠密态 frontier_g=组内行数，顺序 append 活跃组尾部（与基线 insert_vertex_pk_internal 行为一致）。
- frontier 随 checkpoint 落盘（manifest 标量或组元数据），重开恢复。
- 验收：初始稠密态 insert 行为与基线一致；并发分配不产生重复 vid。

## [F013-T502] insert 路径接入前沿分配 + 异 PK 解绑

**description**: insert 路径改用每组前沿分配；压实后前沿回退；异 PK 复用组尾 vid 解绑旧 oid；同 PK 重插不变（FR-020~023）。

**details**:
- src/storages/graph/vertex_table.cc insert_vertex_pk_internal（L188）接入 GroupFrontier 分配。
- 压实后 frontier_g=valid_count（回退）；后续 insert 从前沿顺序回填组尾连续空洞。
- 异 PK 复用组尾 vid：若残留旧 oid→vid 绑定，先从 indices 移除旧绑定，再插新 oid→vid（FR-021）。
- 同 PK 重插复用原 vid（基线行为，不变）；回填保持组内偏移单调，不抢压实前散布异 PK 空槽。
- MVCC：新点按新时间戳判定，旧快照不读新点。
- 验收：压实后 insert 落进组尾洞；异 PK 复用后旧 PK 查找不指向新点；MVCC 可见性正确。

## [F013-T503] M5 前沿分配与回填测试

**description**: 覆盖每组前沿分配、前沿回退、异 PK 解绑、同 PK 重插、回填闭环、MVCC 可见性（FR-019~023）。

**details**:
- tests/storage/test_row_group_compaction.cc：前沿分配/回退；异 PK 解绑（旧 PK 不指向新点）；同 PK 重插；
  回填闭环（重排→insert→再重排）；初始稠密态基线一致；MVCC 可见性。
- 验收：FR-019~023 可验证。