# Module 1: 统一行组边界与组算术

**Goal**: 为一张表的所有列确立统一每组行数 rows_per_group（默认 262144），base_g=g×G 均匀，组边界由纯算术唯一确定，
一个组恒等于整数个逻辑 chunk。交付 RowGroupLayout 布局契约与 ChunkedColumn 的布局承载，是其余模块的地基。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T101] RowGroupLayout 布局契约与默认常量

**description**: 新增纯算术组布局描述符 RowGroupLayout 与默认每组行数常量，提供组算术与校验，供 ChunkedColumn 持有。
预期：组边界可由纯算术唯一确定，非法组大小被拒（FR-001~004 的算术基础）。

**details**:
- 新建 `include/neug/storages/chunk/row_group.h`（header-only，仅依赖 `<cstdint>`，不依赖 Table/Column）：
  - `struct RowGroupLayout { uint64_t rows_per_group; }`，`explicit RowGroupLayout(uint64_t G = kDefaultRowsPerGroup)`。
  - 算术方法（const、无分配）：`GroupOf(vid)=vid/G`；`OffsetOf(vid)=vid%G`；`GroupBegin(g)=g*G`；
    `GroupEnd(g, table_size)=min((g+1)*G, table_size)`；`GroupCount(table_size)=(table_size+G-1)/G`。
  - `static Validate(uint64_t G, uint64_t rows_per_chunk)`：G!=0 且 2 的幂、G>=rows_per_chunk、G%rows_per_chunk==0，
    否则 `THROW_INVALID_ARGUMENT_EXCEPTION`。
- `include/neug/storages/chunk/chunk_types.h`：加 `inline constexpr uint64_t kDefaultRowsPerGroup = 262144;`
  （2^18，各列 chunk 行数的最小通用公倍数，bool 宽度 1 达上限）。
- 例：G=8、vid=11 → GroupOf=1、OffsetOf=3、GroupBegin(1)=8；size=20 → GroupCount=3（组2 尾组 [16,20)）。
- 不引入物理容器/组目录；本头文件是纯算术契约。
- 验收：组算术边界（0、G±1、尾行）正确；Validate 对 G=12（非 2 幂）、G<chunk、不整除 抛异常。

## [F012-T102] ChunkedColumn 承载布局与组访问器

**description**: 让 ChunkedColumn 持有 RowGroupLayout，构造时校验组大小，暴露 rows_per_group()/group_count()。
预期：列携带统一组边界，热路径不变。

**details**:
- `include/neug/utils/property/chunked_column.h`：
  - 构造函数改双参：`explicit ChunkedColumn(size_t rows_per_chunk, size_t rows_per_group = kDefaultRowsPerGroup)`；
    初始化列表在 `ValidateRows(rows_per_chunk)` 之后构造 `group_layout_`（先 `RowGroupLayout::Validate(rows_per_group, rows_per_chunk)`）。
  - 加成员 `RowGroupLayout group_layout_;`（承载组算术，替代裸标量）。
  - 加访问器：`size_t rows_per_group() const { return group_layout_.rows_per_group; }`；
    `size_t group_count() const { return group_layout_.GroupCount(size_); }`。
  - **热路径一行不改**：get_view/get_span/set_value/PrepareForInsert/InsertValue 仍用 `pages_[row>>page_shift_]`。
  - include `neug/storages/chunk/row_group.h`。
- 默认参数保证 `CreatePropertyColumn` 无参工厂（src/utils/property/column.cc）仍可用（本特性不改工厂签名）。
- 验收：chunk=4/G=8 构造合法；chunk=4/G=12 抛；rows_per_group()/group_count() 正确。
- 依赖：T101。

## [F012-T103] M1 组算术与校验单元测试

**description**: 覆盖组算术边界、整除约束、跨列对齐。预期：FR-001~004 可验证。

**details**:
- `tests/storage/test_chunked_column.cc` 加 TEST（并入 chunk_foundation_test，复用 fixture kRowsPerChunk=4）：
  - 组算术：G=8，vid=0/7/8/9/尾行 的 GroupOf/OffsetOf/GroupBegin/GroupEnd；size=0/1/20 的 GroupCount。
  - 整除校验：非法 G（非 2 幂 / <chunk / 不整除）构造抛异常。
  - 跨列对齐：用小参数模拟（chunk=4 与 chunk=2、G=8），任意 vid 两列 GroupOf 一致。
- 验收：`GLOG_minloglevel=3 ./build/tests/storage/chunk_foundation_test` 通过。
- 依赖：T101、T102。