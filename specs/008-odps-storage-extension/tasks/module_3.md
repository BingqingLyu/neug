# Module 3: 读取下推与并行 (Priority: P3)

**Goal**: 把列裁剪、分区裁剪、谓词过滤尽量下推到 ODPS 侧，并按分片并行读取，降低传输量与耗时且结果等价。
依赖 Module 1 读取链路稳定。对应 FR-011~015，SC-004/005。

**Assignee**: [TBD — 需确认]
**Label**: [TBD — 建议 `extension` / `odps`]
**Milestone**: [TBD]
**Project**: [TBD]

## [F008-T301] 列裁剪下推

**description**: 只从 ODPS 请求查询实际需要的列，未引用列不传输（FR-011，plan 算法 5）。

**details**:
* 把 `project_columns`（来自 `ScanFileBindData::getProjectColumns`）填入 SDK `FilterOptions.mRequiredDataColumns`，在 `BuildTableReadSession().SetFilterOptions(...)` 时生效。
* 无投影信息时回退全列（保持正确性）。
* 验证：多列大表只 `RETURN` 两列时，传输量显著下降、结果正确（Acceptance 1）。

## [F008-T302] 分区裁剪下推

**description**: 把分区限定下推到 ODPS，仅扫描相关分区；未限定则读全部分区（FR-012，M1 T103 的分区解析延伸）。

**details**:
* 地址/选项的分区（`pt=...,ds=...`）→ `FilterOptions.mRequiredPartitionColumns` + `mRequiredPartitions`（`mCrossPartition` 按需）。
* 命中 0 分区 → `GetSplitCount()==0`，supplier 返回空结果（Edge Case）。
* 验证：限定单分区时仅该分区被读取（Acceptance 3）。

## [F008-T303] 谓词下推翻译

**description**: 把可下推的 `skip_rows` 过滤表达式翻译为 SDK `IPredicate`，交给 ODPS 侧过滤（FR-012，plan 算法 5）。

**details**:
* 文件：`include/odps_predicate_converter.h` + `src/odps_predicate_converter.cc`。
* 白名单：`=`/`!=`/`>`/`<`/`>=`/`<=` → `IBinaryPredicate`；`IN/NOT IN` → `IInPredicate`；`IS NULL/NOT NULL` → `IUnaryPredicate`；`AND/OR/NOT` → `ICompoundPredicate`；列 → `IAttribute`，常量 → `IConstant`；或整体 `IRawPredicate` 经 `SetFilterPredicate(str)` 承载。
* 递归遍历 `skip_rows` AST：仅纳入白名单子树；含函数/复杂算子的子树不纳入下推（留给 T304 回退）。

## [F008-T304] 下推回退与结果等价对拍

**description**: 对无法下推的谓词回退到引擎侧过滤，保证最终结果与未下推完全一致（FR-013/015，SC-004）。

**details**:
* 翻译失败或 SDK 拒绝谓词 → 只下推列/分区裁剪，过滤交 NeuG 现有 filter 算子。
* 结果等价：下推路径结果集 ≡ 全表扫描 + 引擎过滤（对拍行数与逐列值）。
* 观测传输量/读取行数下降指标（SC-004）。

## [F008-T305] 分片并行读取

**description**: 按数据量/行数把表划分为多 split 并支持并行读取，分片粒度可配置（FR-014，SC-005）。

**details**:
* `SplitOptions{mSplitMode=SIZE, mSplitSize}`（或按行），`mSplitSize` 由 `split_size_mb` 选项控制；`ReadOptions.mMaxBatchRows`（默认 4096）控制单批行数。
* 并行：多 split 读取分派到执行侧并行度（对齐 NeuG 现有并发模型，不阻塞主流程）。
* 配额/限流（Edge Case）：并发受限时分片 + 限速稳定完成。
* 验证：并行分片读取结果与串行一致，总时延下降。

## [F008-T306] Module 3 单测 + 集成测

**description**: 覆盖下推判定、裁剪集合计算与结果等价（对齐 spec M3 Test Strategy）。

**details**:
* 单测（无网络）：可/不可下推谓词判定、列裁剪集合计算、分片划分参数、AST→IPredicate 翻译。
* 集成测：下推前后结果一致性对拍；传输量/读取行数下降的可观测验证；并行分片读取正确性。
