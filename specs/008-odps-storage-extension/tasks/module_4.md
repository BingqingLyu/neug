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

## [F008-T402] 精度与时区约定

**description**: 明确并一致处理高精度小数、日期时间/时间戳、区间的单位/时区与溢出/超范围行为，并写入文档（FR-019）。

**details**:
* 约定：DECIMAL 溢出→报错；TIMESTAMP 超出可表示范围→明确截断或报错；datetime/timestamp 统一 UTC 单位（ms/ns）；interval 承载方式对齐现有 parquet 处理。
* 在 extension 文档/注释固化约定（供用户预期）。
* 边界值单测覆盖（最大值/最小值/时区切换）。

## [F008-T403] 聚簇表 / Delta / 物化视图读取支持验证

**description**: 验证对标准表、分区表之外的聚簇表、Delta 表、物化视图读取正确（FR-016，C4）。

**details**:
* 聚簇表：确认 `mRequiredBucketIds`/桶裁剪语义不影响全量读取正确性。
* Delta / 物化视图：建 `TableReadSession` 读取路径覆盖并端到端比对。
* 覆盖各类表的样表集成测（与 T401 全类型样表区分：此处按表类型维度）。

## [F008-T404] 不支持项显式报错

**description**: 对外部表、逻辑视图、JSON 列返回明确"不支持"错误，指明原因 + 可行替代路径，绝不静默产出错误数据
（FR-018，C4，SC-002）。

**details**:
* 在 sniff/建会话阶段识别表类型（外部表/视图）或列类型（JSON）→ 抛带原因错误。
* 替代路径提示：外部表可改读其底层对象存储文件（OSS 上 Parquet/CSV），经 httpfs + parquet/csv extension。
* 源于 Storage API 自身限制（C4），错误信息须说明这点。

## [F008-T405] Module 4 单测 + 集成测

**description**: 覆盖完整类型映射、精度/时区边界与不支持项（对齐 spec M4 Test Strategy）。

**details**:
* 单测（无网络）：每个 MaxCompute 类型→NeuG 映射、精度/时区边界值、不支持项错误路径。
* 集成测：全类型样表端到端读取比对；外部表/逻辑视图/JSON 的拒绝行为验证；聚簇表/Delta/物化视图读取。
