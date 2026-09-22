# Module 1: ODPS 表扫描数据源 (Priority: P1)

**Goal**: 让用户像使用其它数据源一样，用 `LOAD FROM "odps://<project>/<table>" RETURN ...` 直连读取一张
MaxCompute（ODPS）表为 NeuG 内可查询的列式数据。含 extension 脚手架与 SDK 集成等地基工作。对应 FR-001~006。

**Assignee**: [TBD — 需确认]
**Label**: [TBD — 建议 `extension` / `odps`]
**Milestone**: [TBD]
**Project**: [TBD]

## [F008-T101] 集成 aliyun-odps-sdk-cpp 为 third_party

**description**: 把官方数据面 C++ SDK `aliyun-odps-sdk-cpp` 以对齐 carquet 的方式并入 NeuG 构建，产出可被
odps extension 链接的目标（含 `max_storage_api` 模块、Arrow 支持开启），NeuG core 不受影响。

**details**:
* 在 `third_party/aliyun-odps-sdk-cpp` 添加 git submodule（Apache-2.0），可选 `third_party/odps-sdk.patch`（幂等 `git apply --check` 模式，参照 `extension/parquet/CMakeLists.txt::build_carquet_as_third_party`）。
* 新增 `cmake/BuildOdpsSdkAsThirdParty.cmake`（遵循"每库一文件"约定）：`add_subdirectory(... EXCLUDE_FROM_ALL)`，关闭 SDK 的 test/example/benchmark，设置 `WITH_ARROW=ON`（定义 `ODPS_SDK_ENABLE_ARROW`）、复用 system-first 依赖（protobuf/curl/OpenSSL/zstd/lz4，Boost 仅头文件）。
* 在根 `CMakeLists.txt` 引入该 Build 脚本，导出 odps extension 需要的 SDK/Arrow include 与库变量（类比 `ARROW_INCLUDE_DIRS`/`ARROW_BASE_LIB`）。
* 验证：配置并构建 SDK 目标通过。**风险**：SDK 自带 Arrow 1.0.0（2020）在 C++20 工具链的编译性；若受阻，尝试用 system-first 覆盖指向 NeuG Arrow 18（注意 SDK 端 Arrow API drift），并在本任务记录结论。
* 依赖：git submodule 权限、SDK 依赖的 dev 库（见 `scripts/install_deps.sh`）。

## [F008-T102] 建立 extension/odps 骨架与注册

**description**: 创建与 parquet/httpfs 同构的 extension 目录与 CMake 注册，产出一个可 `LOAD odps;` 加载、
注册了 `ODPS_SCAN` 占位 `ReadFunction` 的空 extension。

**details**:
* 目录：`extension/odps/{CMakeLists.txt, include/, src/, tests/}`。
* `src/odps_extension.cc`：`extern "C" void Init()` 内 `ExtensionAPI::registerFunction<OdpsReadFunction>(CatalogEntryType::TABLE_FUNCTION_ENTRY)` + `registerExtension({"odps", "..."})`；`const char* Name(){return "ODPS";}`（参照 `extension/parquet/src/parquet_extension.cc`）。
* `include/odps_read_function.h`：`OdpsReadFunction : public ReadFunction`，注册名 `ODPS_SCAN`，`sniffFunc/execFunc/supplierFunc` 先返回占位（后续任务填充），使 `format="odps"` 能被 `Binder::getScanFunction` 命中（`{FORMAT}_SCAN` 约定）。
* CMake：`build_extension_lib("odps")`；PRIVATE 链 `neug`（放最后）+ T101 的 SDK/Arrow；在 `cmake/neug_extension.cmake` 的 `NEUG_BUILTIN_EXTENSIONS` 追加 `odps`、`extension/CMakeLists.txt` 加 `add_subdirectory(odps)`（受开关控制，参照 httpfs/parquet）。
* 隔离：确认以 RTLD_LOCAL 加载、符号隐藏（`cmake/neug_exports.ld`/`neug_unexported.sym`），core 不暴露 SDK/Arrow 符号。
* 验收：`make cpp-build` 通过；Python 侧 `LOAD odps;` 成功、`SHOW LOADED_EXTENSIONS()` 可见 `odps`。

## [F008-T103] 实现 odps_options：地址与选项解析

**description**: 把 `odps://[project.][schema.]table[?partitionSpec]` 与 COPY/LOAD 选项解析为内部
`OdpsSourceDesc`（project/schema/table/partitions/连接与分片参数），含默认值规则与非法输入报错（FR-001/003/005）。

**details**:
* 文件：`include/odps_options.h` + `src/odps_options.cc`。
* 解析：去 scheme → 按 `.` 拆最多三段（缺 schema 用 `default`，缺 project 用连接默认 project）；`?` 之后分区串（`pt=...,ds=...`）解析进 `partitions`（映射到后续 `FilterOptions.mRequiredPartitions`）；其余选项（`split_size_mb`、`quota_name` 等）从 `reader::FileSchema.options` 读取。
* 复用现有 `FileSchema{paths,format,protocol,options}`，`format=protocol="odps"`，不新增 proto 字段。
* 错误：缺 table、分区语法非法 → 抛带定位的 `odps_options` 专属异常（`THROW_*`，遵循 neug 异常宏约定）。
* 凭据/endpoint 不进入本描述符（避免入查询文本），交由 T104。

## [F008-T104] 实现 odps_connection：SDK 连接与凭据注入

**description**: 封装 SDK 的 `AliyunAccount`/`Configuration`/`MaxStorageApi.Init`，凭据与 endpoint 解析对齐
httpfs（FR-004/005，SC-006）。

**details**:
* 文件：`include/odps_connection.h` + `src/odps_connection.cc`。
* 复用 `extension/httpfs/src/s3_options.cc` 的模式：`resolveOption(opts,{aliases},{envKeys})`/`findFirstEnv({...})`/`maskCredential()`，优先级 **显式 options > 环境变量 > 报错**。
* AK：options `access_id`/`access_key` 或 env `ODPS_ACCESS_KEY_ID`/`ODPS_ACCESS_KEY_SECRET`（含 `ALIBABA_CLOUD_ACCESS_KEY_ID/SECRET` 别名）；endpoint：options 或 env `ODPS_ENDPOINT`（+ tunnel endpoint）。日志一律 `maskCredential` 脱敏。
* 网络：endpoint 可配置，内网优先但不硬编码"仅内网"（C2）；连接失败给出指向网络/地址配置的错误。
* 构建：`MaxStorageApi api; api.Init(conf);` 供 sniff/read 复用；不打印凭据明文（SC-006）。

## [F008-T105] 实现 sniffFunc + odps_schema_converter（基础标量）

**description**: 实现 `sniffFunc`：建 `TableReadSession` 读 `TableSchema`，把基础标量列（整/浮点/布尔/字符串）
映射为 NeuG 类型，产出 `reader::EntrySchema`（列名/类型/列序与 ODPS 一致，FR-002）。

**details**:
* 文件：`include/odps_schema_converter.h` + `src/odps_schema_converter.cc`。
* 流程：`api.BuildTableReadSession().SetProject/SetSchema/SetTable.Build()` → `TableSchema`（`Column{Name,Type,Comment,Nullable}`）→ 逐列按 plan 算法 2 映射表转换（本任务先覆盖 TINYINT/SMALLINT/INT→INT32、BIGINT→INT64、FLOAT/DOUBLE、BOOLEAN、STRING/VARCHAR/CHAR→STRING）。
* 列名规范化按 NeuG 标识符规则（大小写/特殊字符，Edge Case）。
* 复杂/时间/小数类型留到 M4（T401）；此处对暂未覆盖类型可先返回明确"待支持"错误。
* 验收：对样表 sniff 结果的列名/类型/列序与 ODPS 表定义一致（FR-002）。

## [F008-T106] 实现 supplier/exec + record_batch_supplier + arrow_bridge

**description**: 实现 `supplierFunc`/`execFunc`：逐 split 建 `TableReadStream`、`Read()` 出 `arrow::RecordBatch`、
经 Arrow C Data Interface 转 `DataChunk`，惰性分批（不整表入内存，FR-006，SC-005）。

**details**:
* 文件：`include/odps_record_batch_supplier.h`+`src/odps_record_batch_supplier.cc`、`include/odps_arrow_bridge.h`+`src/odps_arrow_bridge.cc`。
* 读取循环（plan 算法 3）：对 `i in [0, GetSplitCount)`：`stream = session->BuildTableReadStream()->SetSplit(GetSplit(i)).SetReadOptions({mMaxBatchRows=4096}).Build()`；`while(batch=stream->Read())` → 桥接 → `stream->Close()`。
* 桥接（plan 算法 4）：用 extension 内 Arrow 把 `RecordBatch` 导出为 `ArrowSchema`/`ArrowArray`（C ABI），再逐列转 `ValueColumn`（参照 carquet `arrow_c_export/arrow_c_read` + `column_converter`）；只让 core 类型跨边界。参考 `extension/parquet/src/arrow_column.cc::recordbatch_to_value_datachunk`（勿直接跨版本复用其 Arrow C++ 对象）。
* `IDataChunkSupplier::GetNextChunk()` 契约同 parquet：无批返回 `nullptr`；空表/0 分区直接返回空（`GetSplitCount()==0`）。

## [F008-T107] 端到端打通 LOAD FROM ... RETURN

**description**: 串起 T103~T106，使 `LOAD FROM "odps://proj/tbl" RETURN col1, col2, ...` 返回与 ODPS 侧
`SELECT` 一致的结果（本任务不含下推，全表全列读，FR-001，SC-001）。

**details**:
* 打通 binder→gopt（`physical::DataSource`：extension_name/entry_schema/file_schema/project_columns/skip_rows）→ execution `build_read_source` 全链路对 `odps` 生效（查询链路无需改核心代码，仅确保 `ODPS_SCAN` 命中）。
* `execFunc` 物化为 `execution::Context`；`supplierFunc` 供 COPY 融合。
* 验收：`LOAD FROM "odps://proj/tbl" RETURN *` 列名/类型/行数据与 ODPS 一致（Acceptance Scenario 1）。

## [F008-T108] 错误路径与空结果处理

**description**: 覆盖鉴权失败、项目/表不存在、地址非法、网络不可达、空表/命中 0 分区等路径（FR-005，SC-006，Edge Cases）。

**details**:
* SDK `OdpsException`（error code/msg）→ 归因转 NeuG 异常，含可定位原因；0 凭据明文泄露（经 `maskCredential`）。
* 网络不可达（VPC 限制）→ 指向 endpoint/网络配置的错误。
* 空表/0 分区 → 正常返回空结果且 schema 仍可推断（不报错）。
* 会话创建超时/过期 → 合理超时 + 可诊断错误（可选按 session id 重载）。

## [F008-T109] Module 1 单测 + 集成测

**description**: 为 M1 生成可运行的单测与集成测（对齐 spec M1 Test Strategy）。

**details**:
* 单测（`extension/odps/tests/odps_test.cc`，gtest，无网络）：地址/分区解析（合法+非法）、sniff 列名/列型/列序、凭据缺失错误路径、Arrow→DataChunk 桥（用构造的 RecordBatch/IPC fixtures）。
* 集成测：对真实或 Mock/录制回放的 ODPS 表执行 `LOAD FROM ... RETURN` 逐列比对、分区限定读取、鉴权失败与表不存在错误。
* Python e2e：`tools/python_bind/tests/test_load.py` 增加 `LOAD FROM "odps://..."` 用例，`@pytest.mark.skipif`（无凭据/无内网时跳过）。
* 接入 CMake：`add_extension_test(NAME odps ...)`（参照 parquet）。
