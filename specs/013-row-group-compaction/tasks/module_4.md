# Module 4: CSR neighbor 跨组 remap

**Goal**: remap 指向重排组顶点的 CSR neighbor 字段，波及面限于重排组邻居集合（经 g 的 out/in 定位、非全图扫描），批量去重（FR-015~018）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T401] CSR 邻居集合定位

**description**: 经重排组 g 的 out_csr/in_csr 定位 neighbor∈g 的对端（g 的 in/out 邻居），波及面被 g 度数封顶（FR-015、FR-016）。

**details**:
- 在 EdgeTable（include/neug/storages/graph/edge_table.h + src/storages/graph/edge_table.cc）或编排器实现邻居定位。
- 经 g 的 out_csr（neighbor=dst）/in_csr（neighbor=src）列出 neighbor∈g 的对端顶点集合。
- MUST NOT 全图扫描所有邻接记录；波及面被 g 度数封顶。
- 验收：邻居集合定位正确；非邻居组不受影响。

## [F013-T402] CSR neighbor remap 与批量去重

**description**: 改写邻居邻接表里指向 g 旧 vid 的 neighbor 字段为新 vid；批量重排合并去重邻居集合（FR-015、FR-017、FR-018）。

**details**:
- MutableCsr/ImmutableCsr（include/neug/storages/csr/）加 neighbor remap 接口：扫 nbr_t，neighbor∈g 的改为映射后 new_vid。
- 批量重排多个组时合并去重邻居集合，同一邻居邻接表只改写一次。
- MVCC：窗口内已回收旧快照；remap 产生新邻接表版本（COW 单顶点粒度），发布后旧版本随 GC 回收。
- 验收：remap 后图遍历指向正确顶点（跨组/跨 label 边）；批量去重生效。

## [F013-T403] M4 CSR remap 测试

**description**: 覆盖邻居集合定位、neighbor remap、批量去重、跨组/跨 label 边正确、非邻居组隔离（FR-015~018）。

**details**:
- tests/storage/test_row_group_compaction.cc：跨组边重排后遍历正确；跨 label 边；批量重排去重；非邻居组隔离。
- 验收：FR-015~018 可验证。