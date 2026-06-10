# Module 3: 会话级生命周期与 schema 管理

**Goal**: 管理临时图从创建到释放的完整生命周期，确保 Connection 关闭时自动清理、清理后缓存不失效。

**Assignee**: TBD
**Label**: temp-graph
**Milestone**: TBD
**Project**: TBD

## [F008-T301] Connection::Close() 自动清理临时数据

**description**: 在 Connection::Close() 中添加临时 label 清理逻辑，确保 Connection 以任何方式结束时回收所有临时数据。

**details**:
* 文件: `src/main/connection.cc`
* 清理顺序: 先边后点（避免引用完整性问题）
  - 遍历 `schema.get_temporary_edge_labels()` → 逐个 `DeleteEdgeType(src, dst, edge)`
  - 遍历 `schema.get_temporary_vertex_labels()` → 逐个 `DeleteVertexType(label)`
* per-label 错误隔离: 单个 label 清理失败用 try-catch 捕获并 LOG(WARNING)，不阻断其他 label 清理
* 清理后刷新查询缓存:
  - `schema_yaml = graph_.schema().to_yaml()`
  - `query_processor_->clear_cache(schema_yaml)`（或 `global_query_cache_->clear()`）
* 使用 `is_closed_` flag 避免重复清理
* 单 Connection + 同步阻塞 Query() 模型下，Close 被调用时不可能有正在执行的查询
* 依赖: T101 (Schema 查询方法)

## [F008-T302] 生命周期端到端测试

**description**: 编写端到端测试覆盖临时图的完整生命周期：创建、查询、显式释放、Connection 关闭清理、缓存隔离。

**details**:
* 测试场景:
  - LOAD 三个临时类型 → `DROP TABLE TempA` → 仅 TempA 被释放，其他仍可查询
  - LOAD 临时数据 → Close Connection → 重新 Connect → 确认看不到上次临时类型
  - LOAD → 执行查询（生成缓存计划）→ Close → 重新 Connect → 引用同名 label 的查询不命中旧缓存
  - LOAD temp → Checkpoint → 验证磁盘文件不含 temp 数据
  - LOAD temp → 重启数据库 → 验证 temp 数据不存在
  - LOAD 失败（无效文件）→ 验证无残留 schema entry 和 table
  - 非 AP read-write 连接上执行 LOAD NODE TABLE → 验证返回明确错误
* 依赖: Module 1, Module 2, T301
