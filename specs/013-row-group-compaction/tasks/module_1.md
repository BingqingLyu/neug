# Module 1: 按组空洞统计与候选选择

**Goal**: 维护每组空洞率，提供自动（阈值配置）与手动（COMPACT 命令即立即重排）两种触发模式选出重排候选（FR-001~005）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T101] 每组空洞率计算与统计

**description**: 实现每组有效行数与空洞率计算，作为重排触发指标；统计跨 checkpoint/重开保留（FR-001、FR-004）。

**details**:
- 在 VertexTable（include/neug/storages/graph/vertex_table.h + src/storages/graph/vertex_table.cc）加按组统计接口。
- 扫该组时间戳（VertexTimestamp::IsVertexValid）统计 valid_count；hole_rate = 1 − valid_count/G。
- 复用 012 Table::group_count()/group_of() 组枚举；按需计算，不常驻热路径。
- 统计可由时间戳重建，跨 checkpoint/重开保留。
- 验收：删若干点后各组空洞率与暴力扫描时间戳一致；重开后一致。

## [F013-T102] 空洞率阈值配置与自动候选选择

**description**: 暴露空洞率阈值配置；自动调度选超阈值组；insert/update-only 不触发（FR-002、FR-005）。

**details**:
- include/neug/config.h.in 加 compaction_hole_rate_threshold（默认保守，如 0.3）。
- 自动调度：hole_rate ≥ 阈值的组入选候选；按邻居波及面（度数）升序排序，优先低波及组。
- insert/update-only 负载 hole_rate 恒 0，永不触发自动重排。
- 批量预算：一次窗口最多重排 N 组（可配）。
- 验收：低于阈值的组自动不选；阈值可配置生效。

## [F013-T103] COMPACT 命令解析与 admin 路由

**description**: 新增 Cypher 管理命令 COMPACT; / COMPACT TABLE <label>;，立即重排 scope 内有空洞的组（不过阈值），走 admin 路径（FR-003）。

**details**:
- compiler 解析 COMPACT 命令（类比 CHECKPOINT）；新增 AdminType::kCompact。
- src/main/execution_slot.cc 路由 kCompact（类比 L296 kCheckpoint）到压实编排器（M6 T601）。
- 手动命令选 scope 内 hole_rate > 0 的组，MUST NOT 受自动阈值限制；无独立 FORCE 关键字。
- 验收：COMPACT TABLE Person 立即重排 Person 有空洞的组（含低于阈值的组）；COMPACT 重排所有 label 有空洞的组。

## [F013-T104] M1 统计与候选选择测试

**description**: 覆盖空洞率计算、自动阈值选择、手动命令立即重排、统计重建（FR-001~005）。

**details**:
- tests/storage/test_row_group_compaction.cc：空洞率与暴力扫描对拍；自动阈值选择；手动命令不过阈值；重开后统计一致。
- COMPACT 命令端到端（admin 路径）。
- 验收：FR-001~005 可验证。