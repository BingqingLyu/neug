# Module 5: 组级脏度统计（compaction 挂载点，只统计不整理）

**Goal**: 支持按组报告脏页/脏字节统计，作为组结构可用性证明与后续组级 compaction 的输入挂载点。
组隔离、按需计算、写路径零新增开销；**只统计、不整理**（FR-019~022）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T501] GroupDirtyStats 与懒式组级统计

**description**: ChunkedColumn 加按需单遍组级脏统计，不在写路径维护组计数（FR-019/020/021/022）。

**details**:
- `include/neug/utils/property/chunked_column.h`：
  - `struct GroupDirtyStats { size_t dirty_pages = 0; size_t dirty_bytes = 0; };`
  - `std::vector<GroupDirtyStats> group_dirty_stats() const`：`result.resize(group_count())`；
    单遍 `for i in [0, pages_.size())`：`g = group_layout_.GroupOf(i * rows_per_page_)`；
    对 prefix/suffix 各查 `dirty.load(std::memory_order_relaxed)`，脏则 `++result[g].dirty_pages`、`result[g].dirty_bytes += segment->bytes`。
  - 页不跨组（G 整除 rows_per_page，因 G 整除 chunk、chunk 整除 page），每页归属唯一组；复杂度 O(pages)。
  - **不加成员缓存、不在 set_value/InsertValue/PrepareForInsert 维护组计数**；仅维护窗口按需调用。
- 例：chunk=4/G=8/page=2，改 vid=11（页5，组1）→ result[1].dirty_pages=1、dirty_bytes=2*sizeof(T)，其余组 0。
- 验收：组隔离；按需；MUST NOT 触发搬运/重写/回收/重排/回填。
- 依赖：T102。

## [F012-T502] M5 统计测试与写吞吐回归

**description**: 覆盖组隔离、按需、口径对拍、Clone 共享段、写吞吐无回退（FR-019~022、SC-005/006）。

**details**:
- `test_chunked_column.cc` 加 TEST：
  - 只改组1一行 → 组1脏统计非零、其余组 0；多轮散布更新后按组统计与暴力解析对拍。
  - 与现有列级脏统计口径对拍；Clone/快照共享段按本列视图聚合、不重复计共享段。
  - 写入热路径引入前后吞吐基准无回退；任意写入/checkpoint 序列不发生整理动作。
- 依赖：T501。