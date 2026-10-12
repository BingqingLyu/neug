# Module 4: 类型覆盖与不支持项处理 (Priority: P4)

**Goal**: 补全 MaxCompute→NeuG 类型映射矩阵、明确精度/时区约定，验证聚簇表/Delta/物化视图读取，并对不支持的
表/类型给出明确错误。对应 FR-016~019，SC-002。

**Assignee**: [TBD — 需确认]
**Label**: [TBD — 建议 `extension` / `odps`]
**Milestone**: [TBD]
**Project**: [TBD]

## [F008-T401] 补全类型映射矩阵

**description**: 把全部受支持的 MaxCompute 标量与复杂类型正确映射为 NeuG 类型（FR-017，plan 算法 2 完整表）。

**details**:
* 扩展 `odps_schema_converter` + `odps_arrow_bridge` 覆盖：DECIMAL(p,s)→Decimal128、BINARY→Blob/String、DATE→Date32、
  DATETIME→Timestamp(ms,UTC)、TIMESTAMP→Timestamp(ns,UTC)、INTERVAL_*、ARRAY/MAP/STRUCT→List/Map/Struct（受 NeuG 列表约束，不可作主键）。
* 解析带参类型（`DECIMAL(10,2)`、`ARRAY<INT>`）。
* 端到端全类型样表逐列值/类型 100% 命中映射矩阵（Acceptance 1，SC-002）。

**状态（arm64 宿主 SDK-OFF）**：已落地标量映射——整数宽度→INT32/INT64、FLOAT/DOUBLE、BOOLEAN、
STRING/VARCHAR/CHAR→VARCHAR、DATE→date32、DATETIME/TIMESTAMP/TIMESTAMP_NTZ→timestamp(ms)、
**DECIMAL(p,s)→DOUBLE（有损，见 extension/odps/README.md）**；arrow_bridge 新增 `d:p,s` decimal128→double
解码（portable hi/lo，非 __int128）。BINARY/INTERVAL_*/ARRAY/MAP/STRUCT/JSON 一律精确报错（见 T404），
**不做**复杂/嵌套类型映射。单测：`MapsDecimalToDouble`/`MapsTemporalTypes`/`ConvertsDecimal128ToDouble` 等，
odps_extension_test 全绿。全类型样表端到端对拍属 SDK-ON live 验证（见 T405）。

**状态（SDK-ON 容器真连验证，已通过）**：全类型样表 `neug_odps_all_types` 端到端逐列命中——14 列
（TINYINT/SMALLINT/INT→INT32、BIGINT→INT64、FLOAT/DOUBLE、DECIMAL→DOUBLE、BOOLEAN、STRING/VARCHAR/CHAR、
DATE、DATETIME、TIMESTAMP）值全部正确，`test_odps_live_all_types_round_trip` PASSED。真连暴露并修复一处
schema-guard 误报：NeuG 两个 internal→proto temporal 发射器不一致（inferCommonType 发 `temporal.timestamp`、
gopt 发 `temporal.date_time`），且 DATE 的 canonical 是 `temporal.date` 而非 `temporal.date32`；遂将
`convertScalarType` 的 kDate 改为 `mutable_date()`，并新增 `temporalClass()` 归一化 + `sameScalarType` 按
temporal 语义类比较，使 `RETURN *` 不再被误判为 CAST 改写（新增单测 `SameScalarTypeNormalizesTemporalEmitters`）。
odps_extension_test：SDK-OFF 宿主 **97/97**、SDK-ON 容器 **95/95** 全绿（差 2 个
`#if !defined(ODPS_SDK_ENABLE_ARROW)` 的 skeleton-only 用例）。

## [F008-T402] 精度与时区约定

**description**: 明确并一致处理高精度小数、日期时间/时间戳、区间的单位/时区与溢出/超范围行为，并写入文档（FR-019）。

**details**:
* 约定：DECIMAL 溢出→报错；TIMESTAMP 超出可表示范围→明确截断或报错；datetime/timestamp 统一 UTC 单位（ms/ns）；interval 承载方式对齐现有 parquet 处理。
* 在 extension 文档/注释固化约定（供用户预期）。
* 边界值单测覆盖（最大值/最小值/时区切换）。

**状态（arm64 宿主 SDK-OFF）**：约定已固化到代码注释 + `extension/odps/README.md`（Type mapping /
Precision and timezone / Unsupported types）：DECIMAL→DOUBLE 有损（double ~15-17 位有效数字，
DECIMAL(38,18) 高精度值静默丢精度，需用户在 MaxCompute 侧转标量 BIGINT/STRING）；TIMESTAMP 亚毫秒
**截断**（ns/1e6、us/1e3）非四舍五入；秒级 TIMESTAMP 溢出毫秒范围、date64 超 32 位天范围→显式报错。
用户已确认接受 DECIMAL→DOUBLE 方案并要求文档写明。

**状态（SDK-ON 容器真连验证，已修正时区约定）**：真连核实 DATETIME/TIMESTAMP 的时区行为，**推翻了原
「不做时区偏移」的表述**——MaxCompute 按 project 时区（`odps.sql.timezone`，本例 graphscope_dev=Asia/Shanghai
UTC+8）把 DATETIME/TIMESTAMP 存为 instant，Storage API 以带 tz 注解的 Arrow `timestamp` 传输、其 int64 载荷
即 UTC epoch；bridge 读原始 epoch 值渲染为 NeuG 无时区 `TIMESTAMP` 并**丢弃 Arrow tz 注解**，故呈现为 **UTC
挂钟**。实测：插入 `09:15:30`（+08:00）读回 `01:15:30` UTC——**同一 instant、非数据损坏**，`.123` 毫秒保留；
DATE（date32，纯日历日）**不偏移**。已据此修正 `doc/source/extensions/odps.md`、`extension/odps/README.md`
与 `odps_schema_converter.cc` 注释（原「no timezone shift」表述错误），并把
`test_odps_live_all_types_round_trip` 的时间戳断言改为按 project tz→UTC 推导期望 instant。

## [F008-T403] 聚簇表 / Delta / 物化视图读取支持验证

**description**: 验证对标准表、分区表之外的聚簇表、Delta 表、物化视图读取正确（FR-016，C4）。

**details**:
* 聚簇表：确认 `mRequiredBucketIds`/桶裁剪语义不影响全量读取正确性。
* Delta / 物化视图：建 `TableReadSession` 读取路径覆盖并端到端比对。
* 覆盖各类表的样表集成测（与 T401 全类型样表区分：此处按表类型维度）。

**状态**：**DEFERRED 到 SDK-ON 容器轮次**——聚簇表/Delta/物化视图的读取验证依赖真连 MaxCompute 与 SDK
数据面（`ODPS_SDK_ENABLE_ARROW` 门控），arm64 宿主 SDK-OFF 构建不编译、不可验证。

## [F008-T404] 不支持项显式报错

**description**: 对外部表、逻辑视图、JSON 列返回明确"不支持"错误，指明原因 + 可行替代路径，绝不静默产出错误数据
（FR-018，C4，SC-002）。

**details**:
* 在 sniff/建会话阶段识别表类型（外部表/视图）或列类型（JSON）→ 抛带原因错误。
* 替代路径提示：外部表可改读其底层对象存储文件（OSS 上 Parquet/CSV），经 httpfs + parquet/csv extension。
* 源于 Storage API 自身限制（C4），错误信息须说明这点。

**状态（arm64 宿主 SDK-OFF）**：**列类型**不支持项已完成——`unsupportedReason()` 为 BINARY/INTERVAL_*/
ARRAY/MAP/STRUCT/JSON 各给出「NeuG 限制原因 + MaxCompute 侧 workaround」，`convertScalarType` 抛
`InvalidArgumentException` 时带列名 + 类型名 + 原因（单测 `UnsupportedTypeMessageIsActionable` 校验消息含
列名/类型/workaround 关键词）。**表类型**识别（外部表/逻辑视图）**DEFERRED 到 SDK-ON 容器轮次**：
当前 `odps_glue_sniff_schema` 只回传列信息、无表类型字段，需扩 glue + SDK 元数据才能在 sniff/建会话阶段
判别外部表/视图并给出「改读 OSS 底层文件（httpfs + parquet/csv）」的替代路径提示。

**状态（SDK-ON 容器真连验证，列类型已通过）**：`neug_odps_unsupported_json`（payload JSON）真连读取在
schema sniff 阶段即被拒，`test_odps_live_unsupported_column_type_reports_error` PASSED——错误消息含列名
（payload）、类型（JSON）、STRING workaround，证明 T404 列类型拒绝在真连数据面有效。**表类型**识别
（外部表/逻辑视图）仍 DEFERRED（需扩 glue + SDK 元数据回传表类型）。

## [F008-T405] Module 4 单测 + 集成测

**description**: 覆盖完整类型映射、精度/时区边界与不支持项（对齐 spec M4 Test Strategy）。

**details**:
* 单测（无网络）：每个 MaxCompute 类型→NeuG 映射、精度/时区边界值、不支持项错误路径。
* 集成测：全类型样表端到端读取比对；外部表/逻辑视图/JSON 的拒绝行为验证；聚簇表/Delta/物化视图读取。

**状态（arm64 宿主 SDK-OFF）**：无网络单测已完成——新增 `MapsDecimalToDouble`/`MapsTemporalTypes`/
`UnsupportedTypeMessageIsActionable`（converter）+ `ConvertsDecimal128ToDouble`/`RejectsMalformedDecimalFormat`/
`RejectsNon128BitDecimal`（bridge，含 addDecimal128 builder），并同步更新受 DECIMAL/temporal 现被支持影响的
`SupportedScalarClassification`/`UnsupportedTypesThrow`/`ConvertColumnsPropagatesUnsupportedType`/
`RejectsUnsupportedChildFormat`；odps_extension_test SDK-OFF 宿主 97/97 全绿。**集成测（全类型样表端到端、外部表/视图/JSON
拒绝、聚簇/Delta/物化视图）DEFERRED 到 SDK-ON 容器 + 真凭证轮次。**

**集成测 fixture 已写入（gated，待容器验证）**：`tools/python_bind/tests/test_odps_load_from.py` 新增两张 live
fixture 表与对应用例——`neug_odps_all_types`（每种受支持标量类型一列，对齐 node_a.csv 的类型扫描；用例
`test_odps_live_all_types_round_trip` 逐列断言映射后的值，DECIMAL→DOUBLE、DATETIME/TIMESTAMP→ms）与
`neug_odps_unsupported_json`（用例 `test_odps_live_unsupported_column_type_reports_error` 断言 JSON 列报出
含列名/类型/STRING workaround 的可诊断错误）；建表 DDL 已写入模块 docstring，用例经 `NEUG_ODPS_LIVE=1` 门控，
arm64 宿主 SDK-OFF 自动 skip（15 collected / 3 routing passed / 12 skipped）。真连行为（尤其 DECIMAL 的 Arrow
`d:p,s` 编码、DATETIME/TIMESTAMP 的实际 Arrow 类型、JSON 列的 SDK type_code）需容器 + 真凭证核实。

**状态（SDK-ON 容器 + 真凭证，集成测已通过）**：在 arm64 容器 `neug-odps-arm64`（`NEUG_WITH_ODPS_SDK=ON`
构建 `/root/neug-build`）以真凭证跑通 live 集成测——`test_odps_load_from.py` **12 passed / 3 skipped**
（3 个 routing smoke 在 SDK-ON 下按设计自动 skip，避免与 live 重复计数）。其中 T405 的两张 fixture 表
`neug_odps_all_types`（全类型端到端逐列对拍）与 `neug_odps_unsupported_json`（JSON 列拒绝）均 PASSED。
真连核实：DECIMAL 的 Arrow `d:p,s` decimal128 正确解码为 DOUBLE；DATETIME/TIMESTAMP 经 Storage API 为
UTC instant（见 T402 修正）；JSON 列 SDK type_code 触发列类型拒绝。C++ odps_extension_test SDK-ON 95/95。
聚簇表/Delta/物化视图与外部表/逻辑视图仍 DEFERRED（见 T403/T404）。
