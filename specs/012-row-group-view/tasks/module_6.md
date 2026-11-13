# Module 6: 图层端到端集成

**Goal**: 把 RowGroup 视图与组枚举接到 VertexTable/EdgeTable，经 PropertyGraph 现有
get_vertex_table/get_edge_table 按 label 端到端访问组结构，无需新增图层存储（FR-023~025）。
组枚举只是 Table 级能力的图层委托，仍守零寻址改造、零整理动作红线。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T601] VertexTable 组枚举委托

**description**: VertexTable 暴露 row_group_layout/group_count/group_at/group_of，委托属性表 table_，
结果与直接访问该表一致（FR-023）。

**details**:
- `include/neug/storages/graph/vertex_table.h`：get_table() 后加 4 个声明（RowGroupLayout/RowGroup 经 table.h 可见）。
- `src/storages/graph/vertex_table.cc`：include `neug/utils/property/row_group.h`；定义 4 方法委托 table_。
- 验收：与 get_table() 直接访问一致；group_at 越界抛 InvalidArgumentException。
- 依赖：T102、T203。

## [F012-T602] EdgeTable 组枚举委托（bundled 保护）

**description**: EdgeTable 暴露同名访问器作用于 unbundled 边属性表；bundled（table_ 空）group_count=0、
row_group_layout 返回默认、group_at/group_of 抛异常（FR-024）。

**details**:
- `include/neug/storages/graph/edge_table.h`：table() 后加 4 个声明。
- `src/storages/graph/edge_table.cc`：include row_group.h；定义 4 方法，table_ 空时保护。
- 验收：bundled group_count=0 且 group_at/group_of 抛；unbundled 与 table() 一致。
- 依赖：T102、T203。

## [F012-T603] 图层集成测试

**description**: VertexTable/EdgeTable 组委托与表级一致；bundled 保护；经 PropertyGraph getter 端到端（FR-025、SC-010）。

**details**:
- `tests/storage/test_vertex_table.cc`：RowGroupDelegatesToPropertyTable（委托一致）、
  RowGroupEnumerationSmallGroup（替换 G=8 列，多组枚举/定位/尾组）。
- `tests/storage/test_edge_table.cc`：RowGroupBundledHasNoPropertyGroups（bundled 保护）、
  RowGroupUnbundledDelegatesToPropertyTable（AddEdge 后 unbundled 委托一致）。
- PropertyGraph 经现有 get_vertex_table/get_edge_table 透传，无需新增成员。
- 验收：test_vertex_table、edge_table_test 通过。
- 依赖：T601、T602。