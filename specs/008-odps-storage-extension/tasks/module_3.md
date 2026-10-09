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

**status**: DEFERRED (TODO) — 2026-10-09 决定推迟；触发条件：待 NeuG 执行引擎具备并行 scan / morsel 模型后，再实现「并行读取」。

**推迟理由**: 经核实 NeuG 执行引擎全无线程池 / 并行 scan 基础设施（`src/execution` 与 `include/neug/execution` 内 `std::thread` / `std::async` / `ThreadPool` / `parallel` / `morsel` / `std::future` / `tbb` / `omp` 匹配数均为 0）；读取契约为单线程顺序 pull（`execFunc` 返回完整 Context，`supplierFunc` 返回单个 `IDataChunkSupplier` 并经 `GetNextChunk()` 顺序拉取），CSV/JSON/parquet 亦均顺序读取。故 details 中「分派到执行侧并行度（对齐 NeuG 现有并发模型）」当前无对象可对齐；强行并行需在 SDK-gated glue 内自建线程池（宿主 x86_64-only + 门控不可编译验证、SDK 线程安全性未证实、与单线程契约相悖）。FR-014 为 SHOULD、SC-005 已由流式分批满足，整体推迟为 TODO。

**已完成（无需重做）**:
* 分片粒度可配置：`split_size_mb` 选项 → `SplitOptions{mSplitMode=SIZE, mSplitSize}` 已全链路打通（odps_options → supplier → glue）并有单测（`RejectsInvalidSplitSize`）；glue `next_batch` 已按 `splitIndex` 游标顺序遍历多 split。
* SC-005（流式读取、内存不随表行数线性增长）：`GetNextChunk()` 每次拉一批、split 逐个 drain，结构上已满足。

**待办（推迟到 NeuG 支持 parallel scan 后）**:
* `ReadOptions.mMaxBatchRows`（默认 4096，SDK 上限 20000）用户可调选项：glue 侧已就绪（`OdpsGlueReadOptions.max_batch_rows` + open_reader 内 `mMaxBatchRows` 赋值），但 supplier 硬编码 `max_batch_rows = 0`、`OdpsConfigOptionKeys` 无对应 key —— 与并行读取一并实现。
* 真·多 split 并行 fan-out、配额/限流下的并发调度、以及「并行结果与串行一致 + 总时延下降」验证。

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
