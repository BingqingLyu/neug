# Module 2: RowGroup 逻辑视图与表级组枚举

**Goal**: 把一张表表达为有序 RowGroup，每组携带 base_g、逻辑包含该范围内所有列，支持按 vid 定位组、组内按列访问、
每组前沿/有效行数抽象。交付"点集被切分为含列的组"这一表级第一公民视图结构（FR-005~009）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T201] ColumnBase 组大小访问器

**description**: 给 ColumnBase 加 rows_per_group() 虚访问器（默认 0=非分块列），ChunkedColumn override，
供 Table 无 RTTI 派生统一布局。

**details**:
- `include/neug/utils/property/column.h`：ColumnBase（L59-85）加 `virtual size_t rows_per_group() const { return 0; }`。
- `chunked_column.h`：`ChunkedColumn<T>` override `size_t rows_per_group() const { return group_layout_.rows_per_group; }`。
- 非 chunked 列（TypedColumn/list/array/legacy）继承默认 0，Table 派生布局时跳过。
- 验收：chunked 列返回 G，非 chunked 列返回 0。
- 依赖：T102。

## [F012-T202] RowGroup 表级视图头文件

**description**: 新增 RowGroup 轻量值视图，逻辑包含组范围内表的所有列，通过行范围引用现有扁平列（不拥有、不复制）。

**details**:
- 新建 `include/neug/utils/property/row_group.h`（header-only）：
  - `struct RowGroup { const Table* table; RowGroupLayout layout; uint64_t group_id; }`。
  - `begin()=group_id*G`；`end()=min((group_id+1)*G, table->size())`；`row_count()=end()-begin()`；
    `frontier()=end()`（初始稠密态：前沿=有效行数=组内行数）。
  - `column(col_id)` → `table->get_column_by_id(col_id)`；`get(col_id, offset)` → `column->get_any(begin()+offset)`。
  - include `neug/utils/property/table.h`、`neug/storages/chunk/row_group.h`。
- 视图只读；写仍走现有列路径（set_any/PrepareForInsert），不经视图。
- 前沿抽象为 compaction spec 的"前沿回退 + 组尾回填"预留语义接口；本特性不引入物理 per-group 分配单元、不新增存储（FR-009）。
- 例：20 行/G=8，组1 begin=8/end=16/row_count=8/frontier=16；get(age,3) 读 vid=11，与全局 get_any(11) 一致。
- 依赖：T101。

## [F012-T203] Table 组枚举 API

**description**: Table 加无状态组枚举：按需从列派生统一布局并校验跨列一致，按值返回 RowGroup 视图（FR-005/007）。

**details**:
- `include/neug/utils/property/table.h` + `src/utils/property/table.cc`：
  - `RowGroupLayout row_group_layout() const`：扫 columns_ 的 rows_per_group()，非 0 者必须全相等
    （否则 `THROW_CHECKPOINT_EXCEPTION`，防错位组）；全 0（纯变长/legacy 表）→ kDefaultRowsPerGroup。
  - `size_t group_count() const` = `row_group_layout().GroupCount(size())`。
  - `RowGroup group_at(size_t g) const`：g>=group_count() 抛；返回 `{this, layout, g}`。
  - `RowGroup group_of(size_t vid) const` = `group_at(vid/G)`。
  - **不新增 Table 成员**（无缓存，避免与 add_columns/delete_column/SetColumn/Clone 的同步负担）；高频调用方自存布局值。
- 例：bool+INT64 表派生 G=262144；20 行/G=8 → 3 组 [0,8)/[8,16)/[16,20)；group_of(11)=组1；空表 0 组、单行 1 组。
- 验收：跨列一致；不一致拒绝；尾组 end 截断到 size()。
- 依赖：T201、T202。

## [F012-T204] M2 视图与组枚举测试

**description**: 覆盖组枚举、按 vid 定位、组内访问对拍、前沿抽象、不一致拒绝（FR-005~009）。

**details**:
- `test_chunked_column.cc` 加 TEST：
  - 多列（bool+INT64）跨列组对齐；组枚举覆盖 [0,N) + 尾组；group_of 定位；组内 get 与全局 get_any 逐值对拍。
  - 空表/单行；frontier()/row_count() 稠密态相等。
  - 列 rows_per_group 不一致 → row_group_layout() 抛。
  - 集成：点表与 unbundled 边属性表上枚举组并按组读取；引入视图前后读取路径回归。
- 验收：FR-005~009 可验证；chunk_foundation_test 通过。
- 依赖：T203。