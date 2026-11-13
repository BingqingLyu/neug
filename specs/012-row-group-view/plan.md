# Implementation Plan: 行组（Row Group）逻辑视图结构

**Branch**: `012-row-group-view` | **Date**: 2026-11-12 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/012-row-group-view/spec.md`

## Summary

在已落地的定长列 chunked COW（布局 B：扁平 `pages_`、子页级 COW、vid 全局行号）之上，引入 **RowGroup 表级逻辑视图**：
跨列统一每组行数 `rows_per_group`（默认 262144），`base_g=g×G` 均匀纯算术；vid 地址空间连续均匀、有效行允许非全局稠密
（组尾空洞），组尾空洞可复用模型以"每组追加前沿"抽象承载（回填机制属下一个 compaction spec）。交付五个模块：
M1 组边界与组算术、M2 RowGroup 视图与表级组枚举、M3 vid 分解与跨结构映射正确性（以验证为主，零寻址改造）、
M4 组边界持久化（目录 v1 flags 位）、M5 组级脏度统计（懒式、零热路径开销）。

技术路线：**不引入物理组容器、不引入组目录/segment tree、不改列内寻址**（`pages_[vid>>page_shift_]` 一行不动）。
组是算术边界上的逻辑覆盖层；RowGroup 视图通过行范围引用现有扁平列。compaction 动作（gather/remap/回填）整体范围外。

## Technical Context

**Language/Version**: C++20（clang-format style=file）
**Primary Dependencies**: 现有存储栈——`ChunkedColumn<T>`（include/neug/utils/property/chunked_column.h）、
`ChunkDirectory` 编解码（src/storages/chunk/chunk_directory.cc）、`Table`（src/utils/property/table.cc）、
Checkpoint/对象存储/GC；无新增第三方依赖
**Storage**: 列目录 blob（magic NGCD + version 1 + CRC32C trailer）；用 v1 头部 flags 保留位（offset 12）标记
`rows_per_group` 存在性，不 bump 版本号；v2 row_start 随 compaction（范围外）
**Testing**: GTest（`chunk_foundation_test` = test_object_writer.cc + test_chunked_column.cc；
`chunk_fault_recovery_test`）；构建 `cmake --build build -j$(sysctl -n hw.ncpu)`（禁裸 -j）

**已核实的关键事实**（计划依据）：
- 目录 flags 双字（offset 12）Encode 写死 0（chunk_directory.cc L159）、Decode 拒非 0（L103）；全仓库唯一生产写者
  `Builder.Commit`，bit0 空闲；Encode 末尾自校验 Decode（L179）；严格尾部检查 `remaining()!=0`（L149）。
- `ChunkedColumn` 热路径全部 `pages_[row>>page_shift_]`（get_view L227 / get_span L237 / set_value L253 /
  PrepareForInsert L278 / InsertValue L499）；`Clone` 现传单参 `rows_per_chunk_`（L316）。
- `ColumnBase`（column.h L59-85）无 rows_per_group 访问器；`Table` 持 `vector<unique_ptr<ColumnBase>>`，
  `size()` 取 columns_[0]->size()（table.h L112-118）；列创建走 `CreatePropertyColumn`（table.cc L66）。
- 测试 corruption 用例符号化 Encode + blob[24]^=1，无硬编码版本化 blob，加字段安全。

## Project Structure

```text
include/neug/storages/chunk/
├── chunk_types.h              # [改] ChunkDirectory + rows_per_group；kDefaultRowsPerGroup 常量
└── row_group.h                # [新] RowGroupLayout：组大小 + 组算术 + 校验（纯头文件、无依赖）
include/neug/utils/property/
├── column.h                   # [改] ColumnBase 加 virtual rows_per_group()（默认 0）
├── chunked_column.h           # [改] 构造/Open/Clone/Builder 承载 rows_per_group + 组级脏统计
├── row_group.h                # [新] RowGroup 表级视图（引用 Table，逻辑包含各列）
└── table.h                    # [改] 表级组枚举 API（无新增成员，布局按需派生）
src/storages/chunk/
└── chunk_directory.cc         # [改] flags bit0 编解码 + 校验
src/utils/property/
└── table.cc                   # [改] 组枚举实现（派生布局 + 一致性校验）
tests/storage/
└── test_chunked_column.cc     # [改] 目录契约 / 列级 / 表级视图 / 端到端映射测试
```

**Structure Decision**: 改动集中在 9 个文件（2 新头文件 + 7 改）。两个新头文件按依赖方向分层：
`storages/chunk/row_group.h` 只放**纯算术布局契约**（RowGroupLayout，无依赖，被 ChunkedColumn 持有）；
`utils/property/row_group.h` 放**表级视图**（RowGroup，引用 Table）。这符合既有依赖方向
（utils/property → storages/chunk），且让 chunk 基础层不反向依赖 Table。**不新建 CMake 目标**
（头文件-only，测试并入现有 chunk_foundation_test），避免缠上工作区未提交的 tests CMakeLists 改动。

## Data Model

本特性的数据模型：
1. RowGroupLayout（组布局契约，新）
2. RowGroup（表级逻辑视图，新）
3. ChunkDirectory.rows_per_group（持久化字段，扩展）
4. ChunkedColumn 的 rows_per_group_ 与 GroupDirtyStats（列级承载与统计，扩展）
5. ColumnBase::rows_per_group() 访问器（接口，扩展）
6. Table 组枚举 API（无状态派生，扩展）

### 1. RowGroupLayout（include/neug/storages/chunk/row_group.h，新）

**Data Structure**: 纯算术布局描述符，只含一个标量；不是物理容器、无 per-group 目录。

```cpp
inline constexpr uint64_t kDefaultRowsPerGroup = 262144;  // 2^18，各列 chunk 行数的最小通用公倍数
struct RowGroupLayout {
  uint64_t rows_per_group = kDefaultRowsPerGroup;
  // 纯算术：GroupOf(vid)=vid/G；GroupBegin(g)=g*G；OffsetOf(vid)=vid%G；
  //         GroupEnd(g,table_size)=min((g+1)*G, table_size)；GroupCount(table_size)=ceil(table_size/G)
  // 校验：Validate(rows_per_group, rows_per_chunk) —— 2 的幂、>= rows_per_chunk、整除 rows_per_chunk
};
```

例：G=8，vid=11 → GroupOf=1、OffsetOf=3、GroupBegin(1)=8；table_size=20 → GroupCount=3（组2 为尾组，行 [16,20)）。

**Data Access & Update**:
- **构造/校验**：`RowGroupLayout{G}` 构造即校验（非法 G 抛异常）；`Validate` 供列构造与目录 Decode 复用。
- **只读算术**：所有方法 const、无状态、无分配；热路径不经过它（列仍用 page_shift_）。
- **更新**：布局一旦确定不可变；表增长只新增组（GroupCount 随 size 增），base_g 不变。

### 2. RowGroup（include/neug/utils/property/row_group.h，新）

**Data Structure**: 轻量值类型视图，逻辑"包含"该组范围内表的所有列；不拥有列、不复制数据。

```cpp
struct RowGroup {
  const Table* table;        // 引用的表（扁平列仍归 Table 所有）
  RowGroupLayout layout;     // 组布局
  uint64_t group_id;         // 组号
  // begin()=group_id*G；end()=min((group_id+1)*G, table->size())
  // column(col_id) → const ColumnBase*（落回 table->get_column_by_id）
  // get(col_id, offset) → Value（读 vid=begin()+offset）
  // frontier()=end()；row_count()=end()-begin()  （初始稠密态：前沿=有效行数=组内行数）
};
```

例：表 20 行、G=8，组1 → begin=8、end=16、row_count=8、frontier=16；`get(age_col, 3)` 读 vid=11，
与 `table.get_column_by_id(age)->get_any(11)` 逐值一致。

**Data Access & Update**:
- **访问**：经视图按组内偏移读任一列，内部换算 vid=begin()+offset 落回现有列读取（FR-006）。
- **前沿抽象**：`frontier()`/`row_count()` 初始由表 size 算术得出（稠密态二者相等）；为 compaction spec 的
  "前沿回退 + 组尾回填"预留语义接口，本特性不引入物理 per-group 分配单元、不新增存储（FR-009）。
- **更新**：视图本身只读；写仍走现有列路径（set_any/PrepareForInsert），不经视图。

### 3. ChunkDirectory.rows_per_group（chunk_types.h + chunk_directory.cc，扩展）

**Data Structure**: 目录结构加一个标量；序列化用 v1 flags bit0 标记存在性。

```cpp
struct ChunkDirectory { /* 现有字段 */ uint64_t rows_per_group = 0;  // 0 = legacy/未记录 };
// v1 头部：magic(4) version(4) row_width(4) flags(4) rows_per_chunk(8) rows_per_page(8)
//          row_count(8) page_count(8) [rows_per_group(8) 仅 flag 置位] object_count(8) ...
constexpr uint32_t kFlagRowsPerGroup = 1;  // chunk_directory.cc 内部
```

例：G=262144 的列 Dump → flags=1、page_count 后多写 8 字节 262144；legacy 列（rows_per_group=0）→ flags=0，
输出与现格式**逐字节一致**。

**Data Access & Update**:
- **Encode**：flags = `rows_per_group!=0 ? kFlagRowsPerGroup : 0`；置位时在 page_count 后、object_count 前写 8 字节。
- **Decode**：L103 改 `(flags & ~kFlagRowsPerGroup)!=0` 抛；flag 置位则条件读 rows_per_group 并校验
  （PowerOfTwo、>=rows_per_chunk、整除）；未置位则 rows_per_group=0。
- **GC**：`ExtractChunkDirObjectIds` 零改动（object_ids 表位置随条件读自动对齐）。

### 4. ChunkedColumn 承载与 GroupDirtyStats（chunked_column.h，扩展）

**Data Structure**: 列加一个布局成员 + 按需统计返回类型；热路径成员（pages_/page_shift_/size_）不变。

```cpp
// 构造：ChunkedColumn(size_t rows_per_chunk, size_t rows_per_group = kDefaultRowsPerGroup)
// 成员：RowGroupLayout group_layout_;   （替代裸标量，承载组算术）
struct GroupDirtyStats { size_t dirty_pages = 0; size_t dirty_bytes = 0; };
// 访问器：rows_per_group()、group_count()=ceil(size_/G)
// 统计：std::vector<GroupDirtyStats> group_dirty_stats() const;  （长度 group_count()，按 group_id 稠密下标）
```

例：chunk=4、G=8、size=20 的列，改 vid=11 一行 → group_dirty_stats()[1].dirty_pages>=1，其余组为 0。

**Data Access & Update**:
- **构造**：`group_layout_(ValidateRowsPerGroup(rows_per_group, rows_per_chunk))`；默认参数供工厂无参创建。
- **Open**：`rows_per_group_ = d.rows_per_group ? d.rows_per_group : max(kDefaultRowsPerGroup, rows_per_chunk_)`
  （legacy 回退，保证整除）；据此重建 group_layout_。
- **Clone**：改传双参 `(rows_per_chunk_, group_layout_.rows_per_group)`，克隆共享段、布局一致。
- **Builder**：加 rows_per_group 字段，Dump 时填 `d.rows_per_group`，Commit 写入目录。
- **统计**：按需单遍扫 pages_（见算法 4）；**不加成员缓存、不在写路径维护组计数**。

### 5. ColumnBase::rows_per_group()（column.h，扩展）

**Data Structure**: 接口加一个带默认实现的虚访问器，供 Table 无 RTTI 派生布局。

```cpp
class ColumnBase { /* ... */ virtual size_t rows_per_group() const { return 0; }  // 0 = 非分块列 };
// ChunkedColumn<T> override：return group_layout_.rows_per_group;
```

**Data Access & Update**: 只读；非 chunked 列（varchar/list/legacy）返回 0，Table 派生布局时跳过。

### 6. Table 组枚举 API（table.h/.cc，扩展，无新增成员）

**Data Structure**: 表不加存储成员；布局按需从列派生并校验一致性，组视图按值返回。

```cpp
RowGroupLayout Table::row_group_layout() const;  // 扫 columns_ 的 rows_per_group()，非 0 者必须一致
size_t Table::group_count() const;               // ceil(size()/G)
RowGroup Table::group_at(size_t group_id) const; // 越界抛
RowGroup Table::group_of(size_t vid) const;      // group_at(vid/G)
```

例：bool 列（chunk=262144）+ INT64 列（chunk=32768）的表，两列 rows_per_group 均 262144 → 派生布局 G=262144；
任意 vid 在两列归属同一组。若两列 rows_per_group 不一致 → 抛 CheckpointException（防错位组）。

**Data Access & Update**:
- **派生**：遍历 columns_，取首个非 0 rows_per_group 为基准，其余非 0 必须相等；全 0（纯变长/legacy 表）→ 默认 G。
- **无状态**：不缓存布局，避免与 add_columns/delete_column/SetColumn/Clone 的同步负担；调用方如需高频使用可自存布局值。
- **枚举**：group_at/group_of 按值返回 RowGroup 视图；尾组 end 截断到 size()。

## Algorithm Model

本特性的算法模型：
1. 组算术与校验（M1）
2. 目录 flags 位编解码与 legacy 回退（M4）
3. 表级布局派生与组枚举（M2）
4. 懒式单遍组级脏统计（M5）
5. 跨结构映射正确性验证（M3，零寻址改造）

### 1. 组算术与校验

**Algorithm Target**: 由纯算术唯一确定组边界，保证跨列对齐、组=整数个逻辑 chunk，非法组大小被拒。

**Algorithm Details**:
- `GroupOf(vid)=vid/G`、`OffsetOf(vid)=vid%G`、`GroupBegin(g)=g*G`、`GroupEnd(g)=min((g+1)*G,size)`、
  `GroupCount(size)=(size+G-1)/G`。G 为 2 的幂时除法/取模可编译为移位/掩码。
- `Validate(G, rows_per_chunk)`：G!=0 且 2 的幂、G>=rows_per_chunk、G%rows_per_chunk==0；任一不满足抛异常。
- 例：G=8、chunk=4 → 合法（一组=2 chunk）；G=8、chunk=16 → 拒（G<chunk）；G=12 → 拒（非 2 的幂）。
- 边界：vid=G-1→组0 偏移 G-1；vid=G→组1 偏移 0；size=0→GroupCount=0。

### 2. 目录 flags 位编解码与 legacy 回退

**Algorithm Target**: 持久化 rows_per_group 且向后兼容——legacy 目录可读、RPG=0 输出逐字节不变、GC 不受影响。

**Algorithm Details**:
- Encode：flags 由 rows_per_group 是否为 0 决定 bit0；置位时 page_count 后插 8 字节。末尾自校验 Decode 保证写读同源。
- Decode：flags 未知位（`& ~kFlagRowsPerGroup`）非 0 抛；bit0 置位则条件读 + Validate；否则 rows_per_group=0。
  严格尾部检查不变（条件读后 remaining 必须为 0）。
- Open 回退：`d.rows_per_group ? d.rows_per_group : max(kDefaultRowsPerGroup, rows_per_chunk_)`。
  例：legacy INT64 列 chunk=32768 → 回退 262144；自定义大 chunk=524288 的旧目录 → 回退 524288（保整除）。
- 例（往返）：G=262144 列 Dump→Open，组划分一致；legacy 目录 Open→回退→再 Dump，flags 置位、组信息落盘。

### 3. 表级布局派生与组枚举

**Algorithm Target**: 从列派生统一布局并校验跨列一致，把表表达为有序 RowGroup 序列（含尾组），按 vid 定位组。

**Algorithm Details**:
- 派生：扫 columns_ 的 rows_per_group()；非 0 值必须全相等（否则抛）；全 0 → kDefaultRowsPerGroup。
- 枚举：group_count=ceil(size/G)；group_at(g) 构造视图 {this, layout, g}，end 截断到 size；group_of(vid)=group_at(vid/G)。
- 例：20 行、G=8 → 3 组：[0,8)、[8,16)、[16,20)；group_of(11)=组1；尾组 row_count=4。
- 空表 size=0 → 0 组；单行 → 1 组（尾组 row_count=1）。

### 4. 懒式单遍组级脏统计

**Algorithm Target**: 按组报告脏页/脏字节，组隔离、按需计算、写路径零新增开销。

**Algorithm Details**:
- `group_dirty_stats()`：result.resize(group_count())；单遍 `for i in [0,pages_.size())`：
  `g = group_layout_.GroupOf(i * rows_per_page_)`；对 prefix/suffix 各查 `dirty.load(relaxed)`，
  脏则 `++result[g].dirty_pages; result[g].dirty_bytes += segment->bytes`。
- 页不跨组（G 整除 rows_per_page，因 G 整除 chunk、chunk 整除 page），故每页归属唯一组。
- 复杂度 O(pages)；仅维护窗口调用；**不缓存、不在 set_value/InsertValue/PrepareForInsert 维护组计数**。
- 例：chunk=4、G=8、page=2，改 vid=11（页5，组1）→ result[1].dirty_pages=1、dirty_bytes=2*sizeof(T)，其余组 0。
- Clone 共享段：按本列 pages_ 视图聚合，共享段在各列视图分别计（口径与现有列级脏统计一致）。

### 5. 跨结构映射正确性验证（M3，零寻址改造）

**Algorithm Target**: 保证 vid=base_g+offset 分解在 CSR neighbor/indexer/时间戳/accessor 中映射一致，含跨组边与空洞。

**Algorithm Details**:
- **无新寻址算法**：布局 B 下 vid 仍是全局行号，所有结构本就按 vid 下标；分解是算术视图（算法 1），
  各结构存储值不变、列方法不改。正确性由"vid 即全局行号"的既有不变量保证，本特性以测试显式验证（FR-015 不以稠密免除）。
- 验证路径：跨组边 v4→v1（组1→组0）读 out_csr[4].neighbor 分解为 base_0+1 解析到组0 偏移1，属性/时间戳/索引一致；
  新插 v6 的 keys[6]/inserted[6]/accessor[6] 与属性第 6 行一致；删点空洞按可见性跳过、映射仍一致；同 PK 重插复用 vid 映射一致。
- 非稠密有效行：地址空间与 base_g 不变，读取按 timestamp 可见性跳过空洞（现有机制），不要求全局稠密（FR-011）。

## Verification / Test Plan

并入 `chunk_foundation_test`（test_chunked_column.cc），复用现有 fixture（CheckpointManager staging + temp dir，kRowsPerChunk=4）：

- **目录契约（M4）**：rows_per_group 往返；RPG=0 输出与现格式逐字节一致；legacy 目录回退；未知 flags 位拒绝；
  非法 RPG（非 2 的幂 / <chunk / 不整除）拒绝；GC 对象引用解析不变；corruption（blob[24]^=1）仍抛。
- **列级（M1/M5）**：chunk=4、G=8 小参数；组算术边界（0、G±1、尾行）；组隔离脏统计；Clone 布局一致 + 共享段统计口径；
  Open 回退（含大 chunk）；group_count/rows_per_group 访问器。
- **表级视图（M2）**：多列（bool+INT64）跨列组对齐；组枚举覆盖性 + 尾组；按 vid 定位组；组内按列访问与全局访问逐值对拍；
  空表/单行；前沿/row_count 抽象；列 rows_per_group 不一致拒绝。
- **端到端（M3）**：双顶点 label + unbundled 边 label（带边属性）；跨组边 neighbor 解析；插入/删除后跨结构映射一致；
  同 PK 重插映射一致；边属性列目录含 rows_per_group；引入前后查询结果回归。
- **回归**：`chunk_fault_recovery_test` 全量；现有 storage/transaction 测试无回退。

**构建验证**：
```bash
cmake --build build -j$(sysctl -n hw.ncpu) --target chunk_foundation_test chunk_fault_recovery_test
GLOG_minloglevel=3 ./build/tests/storage/chunk_foundation_test
GLOG_minloglevel=3 ./build/tests/storage/chunk_fault_recovery_test
cmake --build build -j$(sysctl -n hw.ncpu)   # 全量
make format-check
```

## Delivery / Git

- 分支 `012-row-group-view`（已从 opt-cow HEAD 切出）。
- **选择性提交**：仅本计划触碰的 9 个文件 + spec.md/plan.md；工作区无关改动（PK 预占修复、mvcc_insert、
  tests CMakeLists PK 注册段、submodule 脏指针、未跟踪文档）一律不进提交。`git add` 逐文件、`git diff --cached` 核对。
- 提交风格 `feat(storage): ...`；推送 origin（BingqingLyu fork）；不混进 #1083。
- 审阅方式：默认 workspace 本地审阅（与 spec 一致），推 GitHub 需另行征得同意。

## Assumptions & Boundaries

- 默认 G=262144 对含 bool 列的 schema 是必要最小公倍数；无窄类型列的 schema 未来可配置更小值（本特性不做配置旋钮）。
- rows_per_group 构造参数默认值供工厂无参创建；小参数（chunk=4/G=8）仅测试用。
- 表级布局按需派生、无缓存（避免列增删同步负担）；高频调用方自存布局值。
- 组尾空洞回填机制、组选择策略、并发协调、CSR remap、indices 重建均属下一个 compaction spec；本特性零整理动作。
- build/ 已含测试目标；头文件-only 新增不新建 CMake 目标。