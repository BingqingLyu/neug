# Module 2: 统一 Cypher 查询接口

**Goal**: 验证共享存储方案下，临时图数据通过现有 Cypher 查询路径自动可见，包括纯临时查询、纯持久化查询和混合查询，无需修改任何查询算子。

**Assignee**: TBD
**Label**: temp-graph
**Milestone**: TBD
**Project**: TBD

## [F008-T201] to_yaml() 包含 temporary labels 验证

**description**: 验证 Schema::to_yaml() 在包含 temporary labels 时能正确输出完整 schema（含 temporary 标记），确保 compiler 的 Catalog 刷新机制能看到临时类型。

**details**:
* 此任务主要是验证性工作，核心实现在 T101（Schema 序列化策略）
* 验证 `to_yaml()` 输出包含 temporary labels 及其 `temporary: true` 标记
* 验证 `QueryProcessor::update_compiler_meta_if_needed()` 在 `create_temp_table` flag 触发后，正确调用 `planner_->update_meta(schema_yaml)` 刷新 Catalog
* 验证刷新后，compiler 能正确解析引用 temporary label 的 Cypher 查询
* 验证 `Serialize()` / `DumpToYaml()` 确实跳过了 temporary labels
* 依赖: Module 1 (T101, T108)

## [F008-T202] 混合查询端到端测试

**description**: 编写端到端测试，覆盖纯临时查询、纯持久化查询和混合查询三种场景，验证查询结果与等价的全持久化查询一致。

**details**:
* 测试场景:
  - 纯临时 MATCH: `MATCH (a:TEMP_PERSON) RETURN a.name`
  - 纯持久化 MATCH: `MATCH (a:PERSON) RETURN a.name`（LOAD AS 后不影响持久化查询）
  - 混合 MATCH: `MATCH (a:TEMP_PERSON)-[:TEMP_KNOWS]->(b:PERSON) RETURN a.name, b.name`
  - 临时图 DML: INSERT/UPDATE/DELETE 临时 label 上的数据，验证立即可见
  - 不存在的临时 label: 查询已释放的临时类型，验证返回"未知 label"错误而非空结果
* 验证结果与用持久化图（同样数据导入持久化）执行同样 Cypher 的结果行级一致（SC-004）
* 依赖: Module 1 全部完成
