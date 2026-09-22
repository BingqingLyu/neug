# Implementation Plan: ODPS/MaxCompute Table Input via Storage API Extension

**Branch**: `008-odps-storage-extension` | **Date**: 2026-09-21 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/008-odps-storage-extension/spec.md`

## Summary

以 NeuG out-of-tree/builtin extension 的形式新增 `odps` 数据源，通过阿里云官方数据面 C++ SDK
`aliyun-odps-sdk-cpp` 的 **max_storage_api（Storage API）** 直读 MaxCompute（ODPS）表，复用 NeuG
现有 extension 数据源框架（`ReadFunction` 的 `sniffFunc/execFunc/supplierFunc`）与 `LOAD FROM ... RETURN` /
`COPY tbl FROM (LOAD FROM ...)` 链路，实现"ODPS 表 → 图数据"的批量导入。v1 只做读取，写路径预留扩展性；
读优先 Storage API（列裁剪/分区裁剪/谓词下推 + Arrow 列式批），Tunnel 作兜底/未来写通道。所有 Arrow/SDK
依赖仅约束在 extension 内并 RTLD_LOCAL 隔离，NeuG core 保持零 Arrow/零 SDK 依赖。

## Technical Context

**Language/Version**: C++20（NeuG 主体）；集成 SDK 为 C++（GCC ≥ 4.9.2，NeuG 用 C++20 需更高，按现有工具链）。

**Primary Dependencies**:
- `aliyun-odps-sdk-cpp`（`/Users/bingqing/Documents/projects/aliyun-odps-sdk-cpp`）的 `max_storage_api` 模块，构建开启 `WITH_ARROW=ON`（`ODPS_SDK_ENABLE_ARROW`），因 `TableReadStream::Read()` 返回 `arrow::RecordBatch`。
- SDK 自带依赖：protobuf 3.7.1、curl、OpenSSL、zstd、lz4、Boost（默认仅头文件）、**Arrow 1.0.0**、nlohmann/json。
- NeuG 既有：Arrow 18.0.0、protobuf 3.21.9、glog、brpc、carquet（Arrow C Data Interface 路径）。

**Storage**: 外部只读数据源 = MaxCompute 表（经 Storage API）；NeuG 侧写入沿用 CSR 图存储的 COPY 批量灌入。

**Testing**: C++ gtest（`extension/odps/tests`）+ Python e2e（`tools/python_bind/tests`）。真实 ODPS 需内网+凭据，
CI 无访问：以 mock/录制回放或 `skipif`（无凭据跳过）保证可重复；纯逻辑（URL 解析、类型映射、谓词翻译、列转换）单测覆盖。

**Resolved Decisions（本轮已定）**：
1. **读路径 = Storage API，且全程基于 SDK**：v1 用 `aliyun-odps-sdk-cpp` 的 `max_storage_api` 模块（`WITH_ARROW=ON`）读表，
   REST/protobuf/签名/Arrow IPC 解码 **全部由 SDK 承担，不自研协议、不用 JNI/子进程桥接 Java/Python**。选此路径即
   **接受在 odps extension 内引入 Arrow**（换取 M3 列/分区/谓词全下推 + 列式吞吐）。Tunnel（行式、零 Arrow）仅作兜底/未来写通道，非 v1 读路径。
2. **Arrow 边界 = extension 内自持 + C Data Interface 桥**：Arrow 只存在于 odps extension（链 SDK 自带 Arrow），
   `RecordBatch→DataChunk` 转换在 extension 内部完成后只让 NeuG core 类型（`DataChunk`/`ValueColumn`）跨边界，
   RTLD_LOCAL 隔离；**NeuG core 仍零 Arrow**。跨版本 ABI 通过 Arrow C Data Interface（`ArrowSchema`/`ArrowArray`，见算法 4）规避。
3. **SDK 交付形态 = 对齐 carquet**：`third_party/` 下 git submodule +（可选）`third_party/odps-sdk.patch`，CMake 内幂等 `git apply --check` 应用，
   新增 `cmake/BuildOdpsSdkAsThirdParty.cmake`（沿用每库一文件的约定），`add_subdirectory(... EXCLUDE_FROM_ALL)` 并关掉 SDK 的 test/example/benchmark。
4. **凭据注入 = 对齐 httpfs `s3_options.cc`**：复用其 `resolveOption(opts,{aliases},{envKeys})` / `findFirstEnv({...})` / `maskCredential()` 三件套，
   优先级 **显式 options > 环境变量 > 报错**；AK 走 `access_id/access_key` options 或 `ODPS_ACCESS_KEY_ID/SECRET`（含 `ALIBABA_CLOUD_` 别名）env，
   endpoint 走 options 或 `ODPS_ENDPOINT`；日志一律脱敏、凭据不进查询文本/proto；TLS CA 沿用 `SSL_CERT_FILE→CURL_CA_BUNDLE→AWS_CA_BUNDLE→distro`。

**Risks（需在实现初 spike 验证，非阻塞决策）**：
- **Arrow 1.0.0 可编译性**：SDK 自带 Arrow 1.0.0（2020），需在 NeuG 的 C++20 工具链验证能否构建；若受阻，考虑让 SDK 改用 NeuG 的 Arrow 18（system-first 依赖覆盖），但有 SDK 端 Arrow API drift 风险。spike 结论决定最终 Arrow 版本，不影响 core 零-Arrow 不变式。
- **谓词下推翻译覆盖率**：`skip_rows` 复杂表达式可能无法整树翻译，需保证回退引擎侧过滤后结果等价（算法 5 兜底）。

## Project Structure

新增集中在 `extension/odps/`，对既有文件仅做最小注册改动。

```text
extension/odps/
├── CMakeLists.txt                    # build_extension_lib("odps")；PRIVATE 链 odps-sdk + Arrow(1.0.0) + neug；RTLD_LOCAL 隔离
├── include/
│   ├── odps_extension.h              # 常量/声明
│   ├── odps_read_function.h          # OdpsReadFunction : ReadFunction（sniff/exec/supplier），注册名 ODPS_SCAN
│   ├── odps_options.h                # odps:// URL + FileSchema.options → 连接描述（project/schema/table/partition/endpoint/quota）
│   ├── odps_connection.h             # 封装 AliyunAccount/Configuration/MaxStorageApi 初始化与会话缓存
│   ├── odps_schema_converter.h       # SDK TableSchema/Column → reader::EntrySchema；MaxCompute 类型 → NeuG DataTypeId
│   ├── odps_record_batch_supplier.h  # IDataChunkSupplier：遍历 splits/streams，把每批转成 DataChunk
│   ├── odps_predicate_converter.h     # NeuG skip_rows 表达式 → SDK FilterOptions 谓词（不可翻译则回退引擎侧过滤）
│   └── odps_arrow_bridge.h           # Arrow RecordBatch → NeuG DataChunk 桥（C Data Interface，见算法 4）
├── src/
│   ├── odps_extension.cc             # extern "C" Init(): registerFunction<OdpsReadFunction>(TABLE_FUNCTION_ENTRY) + registerExtension
│   ├── odps_options.cc
│   ├── odps_connection.cc
│   ├── odps_schema_converter.cc
│   ├── odps_record_batch_supplier.cc
│   ├── odps_predicate_converter.cc
│   └── odps_arrow_bridge.cc
└── tests/
    ├── odps_test.cc                  # gtest：URL/类型映射/谓词翻译/桥接转换（纯逻辑，无网络）；读取端到端用 skipif/mock
    └── CMakeLists.txt

# 注册与集成（既有文件的最小改动）
cmake/neug_extension.cmake            # NEUG_BUILTIN_EXTENSIONS += odps
extension/CMakeLists.txt              # add_subdirectory(odps)（受开关控制，参照 httpfs/parquet）
third_party/aliyun-odps-sdk-cpp        # SDK 并入：git submodule（+ 可选 odps-sdk.patch），对齐 carquet
cmake/BuildOdpsSdkAsThirdParty.cmake  # 新增：以 system-first + 源码兜底方式构建 SDK，锁定 WITH_ARROW=ON/关测试示例
CMakeLists.txt                         # 引入 BuildOdpsSdkAsThirdParty、导出 odps 需要的 Arrow 变量
tools/python_bind/tests/test_load.py  # 新增 COPY FROM (LOAD FROM "odps://...") 用例（无凭据 skipif）
```

**Structure Decision**: 采用**单 extension 目录**（`extension/odps/`），与 `extension/parquet`、`extension/httpfs` 同构：
公共头镜像在 `include/`、实现在 `src/`、测试在 `tests/`。SDK 作为 `third_party/` 依赖构建，`odps` extension 以
`build_extension_lib` 产出，PRIVATE 链接 SDK 与其 Arrow，加载时 RTLD_LOCAL + 符号隐藏（`neug_exports.ld` /
`neug_unexported.sym`），确保 `libneug.so` core 不暴露、不依赖任何 Arrow/SDK 符号。查询链路（binder→gopt→
execution）无需改动：`odps` 只需注册 `ODPS_SCAN` 函数并让 `format="odps"` 可被 `Binder::getScanFunction` 命中。

## Data Model

本特性涉及的数据模型：
1. ODPS 源描述符（`odps://` 地址 + 选项）——复用 `reader::FileSchema`。
2. 表结构 EntrySchema（sniff 产物：列名 + NeuG 类型）。
3. 连接与鉴权配置（账号/endpoint/project/schema）。
4. 读取会话与分片（session id + splits + 每片 stream）。
5. Arrow 批 → NeuG 列式 DataChunk。
6. 下推谓词模型（列裁剪/分区裁剪/过滤）。

### 模型 1：ODPS 源描述符（FileSchema）

**Data Structure**: 直接复用 NeuG 现有 `reader::FileSchema{ paths, format, protocol, options(map<string,string>) }`，
不新增 proto 字段。`format="odps"`、`protocol="odps"`。表标识沿用 GraphScope 语义拆成 project/schema/table/partition。

```json
{
  "paths": ["odps://my_project.default.my_table?pt=20260921"],
  "format": "odps",
  "protocol": "odps",
  "options": {
    "project": "my_project",
    "schema": "default",
    "table": "my_table",
    "partitions": "pt=20260921",
    "split_size_mb": "256",
    "quota_name": "pay-as-you-go"
  }
}
```
> 凭据（accessId/accessKey）与 endpoint **不进入** paths/options（避免入查询文本与日志），由 `odps_connection` 从环境/配置读取。

**Data Access & Update**:
- **解析**：`odps_options.cc` 把 `odps://[project.][schema.]table[?partSpec]` + options 解析成内部 `OdpsSourceDesc`；缺 project 用连接默认 project，缺 schema 用 `default`。
- **只读**：v1 该描述符只用于构造读会话；未来写出会扩展出目标表 + 写模式字段（保留结构可增量扩展，见 spec C5）。

### 模型 2：表结构 EntrySchema（sniff）

**Data Structure**: `odps_schema_converter` 从 SDK 的 `TableSchema`（`Column{ Name, Type, Comment, Nullable }`）
产出 NeuG `reader::TableEntrySchema{ columnNames[], columnTypes[] }`（`columnTypes` 为 NeuG `DataType`）。

```json
{
  "columnNames": ["id", "name", "age", "gmt_create"],
  "columnTypes": ["INT64", "STRING", "INT32", "TIMESTAMP"]
}
```
**Data Access & Update**:
- **推断**：`sniffFunc` 建 read session → 读 `TableSchema` → 逐列按"MaxCompute→(Arrow)→NeuG"映射表转换（Algorithm 2）。
- **校验**：列名大小写/保留字冲突按 NeuG 标识符规则规范化；不支持类型（JSON 列）在映射阶段抛明确错误（spec FR-018）。

### 模型 3：连接与鉴权配置

**Data Structure**: `odps_connection.h` 封装 SDK 的 `AliyunAccount` + `Configuration`（service endpoint、tunnel endpoint、
project）。凭据优先级：显式配置 > 环境变量（如 `ALIBABA_CLOUD_ACCESS_KEY_ID/SECRET`）> 配置文件；均不落查询文本。

**Data Access & Update**:
- **构建**：每次读初始化 `MaxStorageApi api; api.Init(conf);`。
- **网络**：endpoint 可配置以支持内网优先，同时保留公网/代理配置位（spec C2），连接逻辑不硬编码"仅内网"。

### 模型 4：读取会话与分片

**Data Structure**: 对应 SDK 的 `TableReadSession`（`GetSessionId()`、`GetSplits()→{GetSplitCount,GetRecordCount,GetSplit(i)}`）
与每片 `TableReadStream`。NeuG 侧不持久化会话，单次查询内有效；session id 可缓存复用（可选，spec 边界情况）。

### 模型 5：Arrow 批 → DataChunk

**Data Structure**: 复用 NeuG 列式 `DataChunk`（`ValueColumn` 向量）。输入为一批 `arrow::RecordBatch`（或经 C Data
Interface 的 `ArrowArray`/`ArrowSchema`）。参照现有 `extension/parquet/src/arrow_column.cc::recordbatch_to_value_datachunk`
与 `RecordBatchChunkSupplier`。

**Data Access & Update**:
- **取下一批**：`odps_record_batch_supplier.GetNextChunk()` 驱动 split/stream 迭代 → 每批转一个 `DataChunk` → 无批时返回 nullptr（与 parquet supplier 同契约）。

### 模型 6：下推谓词模型

**Data Structure**: 输入为 NeuG 已有的 `project_columns`（include 列，来自 `ScanFileBindData::getProjectColumns`）
与 `skip_rows`（过滤表达式）。输出为 SDK `FilterOptions{ mRequiredDataColumns, mRequiredPartitionColumns,
mRequiredPartitions, mRequiredBucketIds, mPredicate }`，其中 `mPredicate` 用 SDK `IPredicate`
（`IBinaryPredicate/IInPredicate/ICompoundPredicate/IUnaryPredicate/IRawPredicate`）构建，或直接
`SetFilterPredicate(rawString)`。见 Algorithm 3。

## Algorithm Model

本特性算法模型（均落在 extension 内）：
1. `odps://` 地址与选项解析。
2. Schema 嗅探与类型映射。
3. 分批/分片流式读取 + 下推装配。
4. Arrow 批跨版本桥接为 NeuG 列式。
5. 谓词/列/分区下推翻译（含回退）。
6. 不支持项的边界处理。

### 算法 1：地址与选项解析

**Algorithm Target**: 把 `odps://[project.][schema.]table[?partitionSpec]` 与 COPY 选项解析为 `OdpsSourceDesc`。

**Algorithm Details**
- 去 scheme → 按 `.` 拆最多三段（project/schema/table，schema 缺省 `default`，project 缺省取连接默认 project）；
- `?` 之后为分区规格串（`pt=...,ds=...`），解析进 `mRequiredPartitions`；
- 其余选项（`split_size_mb`、`quota_name` 等）从 `FileSchema.options` 读取并填入 SDK `SplitOptions`/`Configuration`；
- 例：`odps://sales.default.orders?dt=20260921` → project=sales, schema=default, table=orders, partitions=["dt=20260921"]。
- 非法（缺 table、分区语法错）→ 抛 `odps_options` 专属错误（spec FR-005）。

### 算法 2：Schema 嗅探与类型映射

**Algorithm Target**: 建 read session 取 `TableSchema`，产出 NeuG `EntrySchema`。

**Algorithm Details** —— 映射表（MaxCompute → Arrow(Storage API) → NeuG）：

| MaxCompute | Arrow | NeuG DataTypeId | 备注 |
|---|---|---|---|
| TINYINT/SMALLINT/INT | Int8/16/32 | INT32（窄化到 INT32）| NeuG 无对应窄整型时统一 INT32/INT64 |
| BIGINT | Int64 | INT64 | |
| FLOAT/DOUBLE | Float/Double | FLOAT/DOUBLE | |
| BOOLEAN | Boolean | BOOL | |
| DECIMAL(p,s) | Decimal128（读为 38,18）| DECIMAL/DOUBLE | 精度约定见 spec FR-019；溢出报错 |
| STRING/VARCHAR/CHAR | Utf8 | STRING | |
| BINARY | Binary | BLOB/STRING | |
| DATE | Date32 | DATE | |
| DATETIME | Timestamp(ms,UTC) | TIMESTAMP | |
| TIMESTAMP | Timestamp(ns,UTC) | TIMESTAMP | 超出范围截断/报错 |
| INTERVAL_* | Month/DayTime | INTERVAL(以 STRING 承载) | 对齐现有 parquet 对 interval 的处理 |
| ARRAY/MAP/STRUCT | List/Map/Struct | LIST/MAP/结构展开 | 受 NeuG 列表类型约束（不可作主键等）|
| JSON | — | — | **不支持读取**：直接抛错（spec FR-018）|

- 逐列 `Column.Type` 解析（含带参类型如 `DECIMAL(10,2)`、`ARRAY<INT>`）→ 目标 NeuG 类型；无法映射者抛"不支持类型"错误。

### 算法 3：分批/分片流式读取 + 下推装配

**Algorithm Target**: 在 `supplierFunc` 下产出一个惰性 `IDataChunkSupplier`，`execFunc` 下产出物化 `Context`。

**Algorithm Details**
1. 由描述符 + 凭据构建 `Configuration` → `MaxStorageApi.Init`。
2. 组装 `FilterOptions`：`mRequiredDataColumns` ← `project_columns`；`mRequiredPartitions` ← 算法 1；谓词 ← 算法 5。
3. `BuildTableReadSession().SetProject().SetSchema().SetTable().SetSplitOptions(SIZE).SetFilterOptions(...).Build()`。
4. `splits = session->GetSplits()`；对 `i in [0, GetSplitCount)`：
   `stream = session->BuildTableReadStream()->SetSplit(GetSplit(i)).SetReadOptions({mMaxBatchRows=4096}).Build()`；
   `while (batch = stream->Read())` → 算法 4 转 `DataChunk` → 交给上层（COPY 融合/物化）。`stream->Close()`。
5. 空表/命中 0 分区：`GetSplitCount()==0` → supplier 直接返回 nullptr（空结果，spec 边界）。
- 惰性：supplier 每次 `GetNextChunk()` 只推进当前 split 的一批，内存不随整表增长（spec SC-005）。

### 算法 4：Arrow RecordBatch → NeuG DataChunk 桥接

**Algorithm Target**: 把 SDK(Arrow) 产出的 `arrow::RecordBatch` 安全转为 NeuG `DataChunk`，且不让 Arrow C++ 类型泄漏到 extension 之外。

**Algorithm Details**（已定方案）：
- odps extension 自持一份 Arrow（链 SDK 的 Arrow），`RecordBatch` 仅在 extension 内部出现；转换产物是 NeuG core 的
  `DataChunk`/`ValueColumn`，**只有 core 类型跨 extension 边界**，配合 RTLD_LOCAL + 符号隐藏，core 零 Arrow。
- **跨版本 ABI**：若需复用现成 Arrow→列式代码或规避 1.0.0 vs 18.0.0 混用，经 **Arrow C Data Interface** 中转——用 extension 内的 Arrow
  把 batch 导出为 `ArrowSchema`/`ArrowArray`（C ABI，版本无关），再逐列转 `ValueColumn`（参照 carquet 的 `arrow_c_export/arrow_c_read` + `column_converter`）。
- 转换范围覆盖标量 + 列表/结构；精度/时区按算法 2 约定。参考实现 `extension/parquet/src/arrow_column.cc::recordbatch_to_value_datachunk`（注意其按 NeuG Arrow 编译，不直接跨版本复用）。

### 算法 5：谓词/列/分区下推翻译（含回退）

**Algorithm Target**: 把 NeuG 的 `skip_rows` 过滤表达式翻译为 SDK `IPredicate`；不可翻译者回退引擎侧过滤，结果不变（spec FR-013/FR-015）。

**Algorithm Details**
- 可翻译白名单：`=`/`!=`/`>`/`<`/`>=`/`<=` → `IBinaryPredicate`；`IN/NOT IN` → `IInPredicate`；`IS NULL/NOT NULL` → `IUnaryPredicate`；
  `AND/OR/NOT` → `ICompoundPredicate`；列 → `IAttribute`，常量 → `IConstant`。整体可 `IRawPredicate("...")` 承载。
- 递归遍历过滤表达式 AST：命中白名单的子树并入下推谓词；含函数/复杂算子的子树保留在引擎侧（NeuG 现有 filter 算子）。
- 列裁剪：`project_columns` → `mRequiredDataColumns`；分区裁剪：地址分区 → `mRequiredPartitions`。
- 兜底：翻译失败或 SDK 拒绝谓词 → 仅下推列/分区裁剪，过滤回退引擎侧。

### 算法 6：不支持项的边界处理

**Algorithm Target**: 对外部表/逻辑视图/JSON 及会话/网络异常给出清晰错误。

**Algorithm Details**
- 建会话/取 schema 时若表类型不受支持（外部表、视图）或含 JSON 列 → 抛带原因 + 替代路径提示的错误（如"外部表请改读其 OSS 文件，经 httpfs+parquet"，spec FR-018）。
- 鉴权失败/项目或表不存在/endpoint 不可达（内网限制）→ 归因错误，凭据 0 泄露（spec FR-005/SC-006）。
- 会话过期/创建超时 → 明确错误或按 session id 重载（spec 边界）。

## Build & Isolation Notes

- `extension/odps/CMakeLists.txt` 参照 `extension/parquet/CMakeLists.txt`：`build_extension_lib("odps")`，PRIVATE 链
  odps SDK 目标与其 Arrow/zstd/lz4/protobuf/curl，`neug` 放最后（parquet 已有"neug 必须排在静态 Arrow 之后"的教训）。
- extension 以 **RTLD_LOCAL** 加载（见现有 `extension.cc`），符号隐藏（`cmake/neug_exports.ld`、`neug_unexported.sym`），
  防止 SDK/Arrow 全局对象与 libneug 冲突（double-free/heap corruption）。
- 用户侧：`LOAD odps;` 后 `COPY tbl FROM (LOAD FROM "odps://..." RETURN ...) `；`SHOW LOADED_EXTENSIONS()` 可见 `odps`。

## Rollout (phased, maps to spec P1→P4)

- **Phase A（P1）**：SDK 并入 + `ODPS_SCAN` 注册 + 算法 1/2/3/4（无下推全表列式读取）→ `LOAD FROM "odps://..." RETURN` 正确读表。
- **Phase B（P2）**：打通 `COPY ... FROM (LOAD FROM odps)`（点表 + 边表 src/dst 主键），复用现有列重映射。
- **Phase C（P3）**：算法 5（列/分区/谓词下推 + split 并行），验证传输量下降且结果等价。
- **Phase D（P4）**：算法 2/6 全类型矩阵与不支持项、错误信息完善；补 Python e2e 与 gtest。
