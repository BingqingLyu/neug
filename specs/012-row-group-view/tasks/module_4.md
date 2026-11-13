# Module 4: 组边界持久化与重开恢复

**Goal**: 列目录持久化每组行数 rows_per_group，使 checkpoint 后重开恢复一致组划分；兼容未记录组信息的旧目录；
不破坏目录校验与 GC 的对象引用解析（FR-016~018）。用 v1 头部 flags 保留位标记存在性，不 bump 版本号。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T401] ChunkDirectory 字段与 flags 编解码

**description**: 目录加 rows_per_group 字段，用 v1 flags bit0 标记存在性，编解码向后兼容（FR-016/018）。

**details**:
- `include/neug/storages/chunk/chunk_types.h`：ChunkDirectory（L68-75）加 `uint64_t rows_per_group = 0;`（0=legacy/未记录）。
- `src/storages/chunk/chunk_directory.cc`：
  - 内部 `constexpr uint32_t kFlagRowsPerGroup = 1;`。
  - Encode（L154-181）：flags 由 `d.rows_per_group!=0 ? kFlagRowsPerGroup : 0`（替换 L159 `Put(out,0,4)`）；
    置位时在 page_count（L163）后、object_count（L164）前 `Put(out, d.rows_per_group, 8)`。
  - Decode（L98-108）：L103 改 `(flags & ~kFlagRowsPerGroup)!=0` 抛；bit0 置位则条件读 rows_per_group（page_count 后）
    并校验（PowerOfTwo、>=rows_per_chunk、整除，复用 RowGroupLayout::Validate）；否则 0。
  - 严格尾部检查（L149 `remaining()!=0`）不变；Encode 末尾自校验 Decode（L179）保证写读同源。
- **RPG=0 时输出与现格式逐字节一致**；`ExtractChunkDirObjectIds` 零改动（object_ids 表位置随条件读自动对齐）。
- 验收：往返一致；RPG=0 逐字节不变；未知 flags 位拒绝；非法 RPG 拒绝。
- 依赖：T101（Validate）。

## [F012-T402] ChunkedColumn Open 回退 / Clone / Builder

**description**: 列 Open 按目录恢复布局（legacy 回退），Clone 传双参，Builder Dump 写入 rows_per_group（FR-016/017）。

**details**:
- `include/neug/utils/property/chunked_column.h`：
  - Open（L70-158）：读 d.rows_per_group 后
    `group_layout_ = RowGroupLayout(d.rows_per_group ? d.rows_per_group : max(kDefaultRowsPerGroup, rows_per_chunk_))`；回退值保整除。
  - Clone（L315-324）：改传双参 `(rows_per_chunk_, group_layout_.rows_per_group)`，克隆共享段、布局一致。
  - Builder（L364-431）：加 rows_per_group 字段；Dump（L160-182）填 `builder->rows_per_group`；
    Commit（L390）设 `d.rows_per_group = rows_per_group`。
- 例：legacy INT64 chunk=32768 → 回退 262144；自定义大 chunk=524288 旧目录 → 回退 524288（保整除）。
- 验收：checkpoint→重开组划分一致；legacy 打开→回退→再 checkpoint 组信息落盘。
- 依赖：T102、T401。

## [F012-T403] M4 目录契约与重开测试

**description**: 覆盖目录契约、legacy 回退、GC、corruption、checkpoint 重开（FR-016~018、SC-004）。

**details**:
- `test_chunked_column.cc` 加 TEST：
  - rows_per_group 往返；RPG=0 输出与现格式逐字节一致；legacy 目录回退（含大 chunk）；未知 flags 位拒绝；非法 RPG 拒绝。
  - GC 对象引用解析不变（ExtractChunkDirObjectIds）；corruption（符号化 Encode + blob[24]^=1）仍抛。
  - checkpoint→重开组划分一致；旧 checkpoint 打开→回退→再 checkpoint。
- `chunk_fault_recovery_test` 全量回归。
- 依赖：T401、T402。