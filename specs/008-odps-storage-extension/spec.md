# Feature Specification: ODPS/MaxCompute Table Input via Storage API Extension

**Feature Branch**: `008-odps-storage-extension`  
**Created**: 2026-09-21  
**Status**: Draft  
**Input**: User description: "为 NeuG 新增一个 odps extension，通过 MaxCompute Storage API（Open Storage）把 ODPS/MaxCompute 表作为输入数据源接入，实现 LOAD FROM \"odps://project/table?partition=...\" RETURN ... 以及 COPY tbl FROM (LOAD FROM ...) 的批量导入。v1 仅读取（保留后续支持 ODPS 写入/导出的可扩展性）。复用 NeuG 现有 extension ReadFunction 框架（NeuG core 不引入 Arrow 依赖）。支持标准表/分区表/聚簇表/Delta/物化视图。"

## Overview

NeuG 目前可通过 extension（parquet、httpfs 等）把外部文件/对象存储作为输入数据源，用
`LOAD FROM "..." RETURN ...` 扫描、并用 `COPY tbl FROM (LOAD FROM ...)` 批量灌入图表。本特性新增一个
`odps` extension，让用户以同样的方式**直连读取阿里云 MaxCompute（ODPS）表**作为图数据的输入来源，
无需先把数据导出成文件再搬运。目标是把 ODPS 中已有的大规模业务数据低成本地引入 NeuG 做图分析与图构建。

本特性 **v1 只做读取（输入）**：不涉及向 ODPS 写数据、不导出 NeuG 数据到 ODPS、不改动 ODPS 表。
但架构上 MUST 为后续支持 ODPS 写入/导出预留可扩展性——数据源抽象与模块划分不应硬编码"只读"假设，
以便未来在不重构接入层的前提下增量加入写出能力（写出/导出能力本身不在本 v1 范围内实现）。

## Functional Modules *(mandatory)*

### Module 1: ODPS 表扫描数据源 (Priority: P1)

**Purpose**: 让用户像使用其它数据源一样，用一条查询直接把 ODPS 表读成 NeuG 内可查询的行列数据，
这是整个特性的地基——只有先能"读到并正确呈现一张 ODPS 表"，后续的灌图、下推优化才有意义。

**Why this priority**: 这是最小可用闭环。单独实现它，用户就能对任意受支持的 ODPS 表执行
`LOAD FROM "odps://<project>/<table>" RETURN col1, col2, ...` 并拿到正确结果，可独立验证与演示。

**Independent Test**: 给定一张受支持的 ODPS 表和有效凭据，执行
`LOAD FROM "odps://project/table" RETURN *`，返回的列名、列类型与行数据与 ODPS 表一致即通过。

**Key Components**:

1. **数据源标识与解析**: 接受形如 `odps://<project>/<table>` 的地址，并支持可选的分区限定
   （沿用 GraphScope 习惯的 `[project.]table[|partitionSpec]` 语义，以 URL/选项形式表达）。
2. **连接与鉴权上下文**: 承载访问 ODPS 所需的连接参数（服务地址、项目、访问凭据、资源配额等），
   凭据支持从环境/配置读取，不在查询文本中明文硬编码。
3. **Schema 推断（sniff）**: 在真正读取前，从 ODPS 表元数据推断列名与列类型，供上层绑定与列裁剪使用。
4. **扫描读取（exec/supplier）**: 以批（列式）方式把表数据读入引擎，作为可被查询算子消费的数据源。
5. **表标识格式约定**: 明确 `odps://` 地址中 project、table、partition 三段的书写与默认值规则。

**Functional Requirements**:

1. **FR-001**: 系统 MUST 支持通过 `odps://<project>/<table>` 形式的地址扫描一张 ODPS 表，并以
   `LOAD FROM ... RETURN ...` 返回其数据。
2. **FR-002**: 系统 MUST 在读取前推断出表的列名与列类型，且推断结果的列顺序、列名与 ODPS 表定义一致。
3. **FR-003**: 系统 MUST 支持在地址或选项中限定分区（partition），仅读取指定分区的数据；未限定时读取全表/全部分区。
4. **FR-004**: 系统 MUST 支持通过连接参数指定服务地址、项目与资源配额，并支持从环境变量/配置读取访问凭据。
5. **FR-005**: 当鉴权失败、项目/表不存在、地址格式非法或网络不可达时，系统 MUST 返回清晰的、可定位原因的错误信息，且不泄露凭据明文。
6. **FR-006**: 系统 MUST 以流式/分批方式读取，避免一次性把整表载入内存导致 OOM。

**Acceptance Scenarios**:

1. **Given** 一张标准 ODPS 表和有效凭据，**When** 执行 `LOAD FROM "odps://proj/tbl" RETURN a, b`，
   **Then** 返回的列名/类型/行数据与 `SELECT a, b FROM proj.tbl` 在 ODPS 侧结果一致。
2. **Given** 一张分区表，**When** 执行带分区限定的 `LOAD FROM "odps://proj/tbl?partition=pt=20260921" RETURN *`，
   **Then** 仅返回该分区的数据。
3. **Given** 错误的项目名或表名，**When** 执行扫描，**Then** 返回明确指出"表/项目不存在或无权限"的错误。
4. **Given** 缺少或错误的凭据，**When** 执行扫描，**Then** 返回鉴权失败错误，且错误信息中不包含 AccessKey 明文。

**Test Strategy**:

- **Unit Tests**: 地址/分区解析（合法与非法输入）、schema 推断结果的列名/列类型/列序、凭据缺失时的错误路径。
- **Integration Tests**: 对一张真实（或 Mock 服务模拟的）ODPS 表执行 `LOAD FROM ... RETURN`，逐列比对数据；
  分区限定读取；鉴权失败与表不存在的错误路径。

---

### Module 2: ODPS → 图表批量导入 (Priority: P2)

**Purpose**: 让用户把 ODPS 表作为点表/边表的数据来源，用一条 `COPY ... FROM (LOAD FROM ...)` 完成批量灌图，
这是"引入 ODPS 数据做图分析"的核心业务价值。

**Why this priority**: 依赖 Module 1 的扫描能力，但一旦具备即可端到端交付"ODPS 数据 → NeuG 图"的完整链路，
是用户最终想要的结果。

**Independent Test**: 先 `CREATE NODE TABLE`/`CREATE REL TABLE`，再
`COPY tbl FROM (LOAD FROM "odps://..." RETURN ...)`, 最后用 `MATCH` 校验点数/边数与属性正确。

**Key Components**:

1. **列映射/重映射**: 支持把 ODPS 表的列映射到点/边表定义的属性列，允许列顺序不同、按需选取子集；
   边表需能把指定列映射为起点/终点主键。
2. **与 COPY 灌图链路对接**: 复用 NeuG 现有的 `COPY tbl FROM (LOAD FROM ... RETURN ...)` 批量写入路径。
3. **主键与类型契合校验**: 当 ODPS 列类型与目标图列类型不一致时，遵循 NeuG 现有的强制转换/报错规则。

**Functional Requirements**:

1. **FR-007**: 系统 MUST 支持 `COPY <node_table> FROM (LOAD FROM "odps://..." RETURN ...)` 把 ODPS 表灌入点表。
2. **FR-008**: 系统 MUST 支持把 ODPS 表灌入边表，并能指定用于起点/终点主键的列。
3. **FR-009**: 系统 MUST 支持列重映射：RETURN 的列顺序/子集可与目标表定义不同，按名或按位正确对应。
4. **FR-010**: 当 ODPS 列类型无法安全转换为目标图列类型时，系统 MUST 报错并指出出错的列与类型，而非静默写入错误数据。

**Acceptance Scenarios**:

1. **Given** 已建好的点表 person，**When** `COPY person FROM (LOAD FROM "odps://proj/person_src" RETURN id, name, age)`，
   **Then** `MATCH (p:person) RETURN count(p)` 等于 ODPS 源表行数，且抽样属性一致。
2. **Given** 已建好的边表 knows，**When** 用 ODPS 表灌入并指定 src/dst 主键列，
   **Then** 边的两端正确连接到对应点。
3. **Given** RETURN 列顺序与目标表定义不同，**When** 执行 COPY，**Then** 按列名/位置正确映射，不发生错位。

**Test Strategy**:

- **Unit Tests**: 列映射/重映射解析、边表 src/dst 主键列绑定、类型不兼容的报错路径。
- **Integration Tests**: ODPS→点表、ODPS→边表端到端灌入 + `MATCH` 校验；列顺序错乱场景；类型不匹配报错场景。

---

### Module 3: 读取下推与并行 (Priority: P3)

**Purpose**: 减少对 ODPS 的数据传输量与读取耗时，让大表接入在成本与时延上可用——把过滤、投影、分区裁剪
尽量下推到 ODPS 侧，并利用数据分片并行读取。

**Why this priority**: 功能正确性由 P1/P2 保证；本模块是性能与成本优化，在基础链路可用后带来显著收益，
但不影响"能否用起来"。

**Independent Test**: 对同一张表，比较开启/未开启下推时扫描返回的行数与传输数据量，验证下推生效且结果正确。

**Key Components**:

1. **列裁剪下推**: 只请求查询实际需要的列（RETURN/目标表用到的列）。
2. **分区裁剪下推**: 把分区限定条件下推到 ODPS，仅扫描相关分区。
3. **谓词下推**: 把可下推的过滤条件（等值、范围、IN、IS NULL 等）交给 ODPS 侧过滤。
4. **分片与并行读取**: 按数据量/行数把表划分为多个分片（split），支持并行读取以缩短总时延。

**Functional Requirements**:

1. **FR-011**: 系统 SHOULD 只从 ODPS 请求查询所需的列（列裁剪），未引用列不传输。
2. **FR-012**: 系统 SHOULD 把分区限定与可下推的过滤谓词下推到 ODPS 侧执行。
3. **FR-013**: 系统 MUST 保证下推不改变最终结果集（下推等价于在引擎侧过滤/投影）。
4. **FR-014**: 系统 SHOULD 支持把表划分为多个分片并行读取，并可配置分片粒度。
5. **FR-015**: 对于无法下推的谓词，系统 MUST 回退到引擎侧过滤，结果保持正确。

**Acceptance Scenarios**:

1. **Given** 一张多列大表，**When** 只 `RETURN` 两列，**Then** 传输的数据量显著小于全列读取，且结果正确。
2. **Given** 带过滤条件的扫描，**When** 谓词可下推，**Then** ODPS 侧返回的行数已过滤，最终结果与引擎侧过滤一致。
3. **Given** 分区表且限定单分区，**When** 扫描，**Then** 仅该分区被读取。

**Test Strategy**:

- **Unit Tests**: 可下推/不可下推谓词的判定、列裁剪集合计算、分片划分参数。
- **Integration Tests**: 下推前后结果一致性对拍；传输量/读取行数下降的可观测验证；并行分片读取的正确性。

---

### Module 4: 类型覆盖与不支持项处理 (Priority: P4)

**Purpose**: 保证 ODPS 各数据类型被正确映射为 NeuG 类型，并对不受支持的表类型/数据类型给出明确边界与错误，
避免静默的数据错误或精度陷阱。

**Why this priority**: P1 已覆盖常用类型的基本读取；本模块补齐完整类型矩阵与边界处理，属于健壮性与完备性增强。

**Independent Test**: 对覆盖各类型的 ODPS 表逐列校验映射结果；对不支持的表/类型验证返回明确错误。

**Key Components**:

1. **类型映射矩阵**: MaxCompute 类型 → NeuG 类型的映射（整数/浮点/布尔/字符串/二进制/日期时间/时间戳/
   高精度小数/数组/映射/结构体/区间等）。
2. **精度与时区约定**: 明确高精度小数、日期时间/时间戳的单位与时区处理，以及可能的精度截断行为。
3. **不支持项的显式处理**: 对不受支持的表类型（外部表、逻辑视图）与数据类型（如 JSON 读取）给出明确错误。

**Functional Requirements**:

1. **FR-016**: 系统 MUST 支持标准表、分区表、聚簇表、Delta 表与物化视图的读取。
2. **FR-017**: 系统 MUST 把受支持的 MaxCompute 标量与复杂类型正确映射为对应的 NeuG 类型。
3. **FR-018**: 对于不受支持的表类型（外部表、逻辑视图）或数据类型（如 JSON），系统 MUST 返回明确的
   "不支持"错误，而非静默失败或产生错误数据。此限制源于 MaxCompute Storage API 自身（见 C4），
   错误信息 SHOULD 指明原因并给出可行替代路径（如外部表改读其底层对象存储文件）。
4. **FR-019**: 对于存在精度/时区语义差异的类型（高精度小数、日期时间、时间戳、区间），系统 MUST 有明确、
   一致的转换约定并在文档中说明。

**Acceptance Scenarios**:

1. **Given** 一张覆盖多种类型的 ODPS 表，**When** 扫描读取，**Then** 每列的值与类型均按映射矩阵正确呈现。
2. **Given** 一张外部表或逻辑视图，**When** 尝试扫描，**Then** 返回明确的"该表类型不受支持"错误。
3. **Given** 含 JSON 列的表，**When** 读取该列，**Then** 返回明确的"JSON 类型不支持读取"错误。

**Test Strategy**:

- **Unit Tests**: 每个 MaxCompute 类型到 NeuG 类型的映射；精度/时区边界值；不支持项的错误路径。
- **Integration Tests**: 全类型样表端到端读取比对；外部表/逻辑视图/JSON 的拒绝行为验证。

---

### Edge Cases

- 目标 ODPS 表为空（0 行）：应正常返回空结果，schema 仍可推断。
- 表只有分区列被选取、或分区限定命中 0 个分区：应返回空结果而非报错。
- 读取会话创建耗时长（大表/文件多，服务端可能转异步）：应有合理超时与可诊断的进度/错误反馈。
- 会话过期（默认 24 小时）：长任务或复用会话时应能感知过期并给出明确错误（可选支持按会话 ID 重载）。
- 网络受限：服务地址仅支持特定网络（如 VPC 内网）时，连接失败应给出指向网络/地址配置的错误提示。
- 单请求吞吐/并发受限（配额限制）：大批量读取应能通过分片与限速策略稳定完成，触发限流时可重试或降级。
- 列名大小写/特殊字符：ODPS 列名与 NeuG 标识符大小写敏感性差异需有一致处理。
- 高精度小数溢出、时间戳超出可表示范围：应有明确的截断/报错约定（见 FR-019）。
- 同一查询混用 odps 源与其它源（如 JOIN 本地图表）：数据源边界应清晰，不相互污染。

## Constraints & Assumptions *(informative)*

> 以下为影响范围与可行性的既有事实与默认假设，供 planning 阶段参考；非功能需求本身。

- **C1（接入方式：使用官方 C++ SDK）**: 采用阿里云官方**数据面 C++ SDK** [`aliyun/aliyun-odps-sdk-cpp`](https://github.com/aliyun/aliyun-odps-sdk-cpp)
  （Apache-2.0；其 example 覆盖 core / tunnel / max_storage_api，即**同时提供 Tunnel 与 Storage API 访问**）。
  由 SDK 负责 REST/protobuf 协议、鉴权签名与（可选）Arrow 批解码。是否开启 SDK 的 `WITH_ARROW` 为 plan 决策——
  走 Storage API 列式 stream 读取通常需 `WITH_ARROW=ON`。无论开关，不变式是：**所有 Arrow/SDK 依赖 MUST 仅约束在
  `odps` extension 内**，以 **RTLD_LOCAL 隔离、符号隐藏**（与现有 parquet extension 链接 Arrow 但隔离的做法一致），
  **NeuG core 保持零 Arrow / 零 SDK 依赖**。需集成其依赖（protobuf/curl/OpenSSL/zstd/lz4/Boost 默认仅头文件；Arrow 可选）。
  注：管控面 OpenAPI C++ SDK `alibabacloud-sdk-cpp/maxcompute-20220104`（Darabonba 生成、依赖 CPPRestSDK）
  面向管理接口、不用于读表数据，**非本特性接入路径**。
- **C2（网络：优先内网，保留其它访问扩展性）**: v1 优先支持阿里云 VPC 内网直连（service / tunnel endpoint）。
  接入层 MUST 保留对公网/代理/专线等其它访问方式的**可扩展性**——endpoint/网络参数应可配置，不把"仅内网"
  硬编码进连接逻辑（其它访问方式本身不在 v1 实现范围）。
- **C3（鉴权与配额）**: 需 AccessKey（建议 RAM 子账号）与已开通的 Storage API 配额（按量付费或独占 DTS 资源组）。
- **C4（支持的表/类型，源于 Storage API 自身限制）**: 支持标准表、分区表、聚簇表、Delta、物化视图。
  **不支持外部表、逻辑视图，且 JSON 类型不支持读取——这是 MaxCompute Storage API 自身的限制（官方文档明确），
  非本特性的设计取舍**：Storage API 以"标准表语义"直读 ODPS 底层列式存储，而外部表的数据实际存放在 ODPS 之外
  （如 OSS/其它数据源）、逻辑视图只是一段查询定义而非物理存储，二者都没有可直读的底层列存数据；JSON 半结构
  类型也未开放经 Storage API 读取。替代路径：外部表可改用它底层指向的对象存储文件（如 OSS 上的 Parquet/CSV），
  经 NeuG 现有 httpfs + parquet/csv extension 读取。
- **C5（v1 只读，保留写出扩展性）**: v1 不实现向 ODPS 写入或导出；但接入层抽象需保留未来增量支持写入/导出
  的空间（见 Overview），不得因 v1 只读而把"只读"固化进数据源接口。
- **A1（复用现有框架，Arrow 只锁在 extension 内）**: 复用 NeuG 现有 extension 数据源框架（ReadFunction 的
  sniff/exec/supplier），产出引擎原生的列式数据（Context），参考 parquet extension 的实现范式。
  **NeuG core 已去除 Arrow 依赖，本特性 MUST NOT 给 core 引入 Arrow/SDK 依赖**：ODPS Storage API 传输格式为
  Arrow IPC（源侧客观事实），Arrow 解码交由所选 SDK（见 C1）处理，并连同 SDK 一起以 RTLD_LOCAL 隔离、
  符号隐藏仅约束在 `odps` extension 内（现有 parquet extension 链接 Arrow 但隔离即为先例）；
  具体开关与集成方式为 plan 阶段决策，spec 层面不绑定实现。
- **A2（列映射）**: 点/边列映射复用现有 `LOAD FROM ... RETURN` + `COPY FROM` 的列绑定/重映射能力，
  不新发明一套映射 DSL（区别于 GraphScope 的 JSON 列映射配置）。
- **A3（不复用 GraphScope MR 路径）**: GraphScope 的 ODPS 接入是把计算下推到 ODPS MapReduce 做离线 SST 构建，
  架构与 NeuG 的"引擎内拉取扫描"模型不同，本特性不复用其 MR 路径，仅借鉴其表标识与列映射语义。
- **A4（读优先 Storage API，Tunnel 兜底/未来写）**: 读路径优先用 SDK 的 **max_storage_api**
  （列裁剪/分区裁剪/谓词下推 + 列式批，直接支撑 Module 3 下推优化）；**Tunnel**（行式、无下推）作为能力兜底，
  并作为未来写出/导出（见 C5）的候选通道。两条路径统一收敛到同一 ReadFunction 抽象之下。

## Resolved Decisions

- **接入方式（原"C++ 接入方式"疑问）**: 已定 —— 使用官方 C++ SDK `aliyun/aliyun-odps-sdk-cpp`，连同其
  （可选）Arrow 支持一起仅约束在 `odps` extension 内、以 RTLD_LOCAL 隔离，NeuG core 保持零 Arrow/零 SDK 依赖
  （`WITH_ARROW` 开关留待 plan 决策，详见 C1）；不再原生实现协议或 JNI 桥接。
- **目标网络（原"网络环境"疑问）**: 已定 —— v1 优先 VPC 内网直连，endpoint 等网络参数可配置以保留
  公网/代理/专线的可扩展性（详见 C2）。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 用户对一张受支持的 ODPS 表执行一条 `LOAD FROM "odps://..." RETURN ...` 即可拿到与
  ODPS 侧 `SELECT` 一致的结果，无需任何中间导出/搬运步骤。
- **SC-002**: 覆盖全部受支持数据类型的样表，扫描读取后每列的值与类型 100% 符合类型映射矩阵；
  对不支持的表类型/JSON 列 100% 返回明确错误而非静默错误数据。
- **SC-003**: 用户可用一条 `COPY <table> FROM (LOAD FROM "odps://..." RETURN ...)` 把 ODPS 表灌入点表/边表，
  灌入后 `MATCH` 校验的点数/边数与源表行数一致，抽样属性一致。
- **SC-004**: 开启列裁剪 + 分区裁剪 + 谓词下推后，相比未下推的全表全列读取，从 ODPS 传输的数据量与
  读取行数显著下降（在样表上可量化验证），且最终结果集与未下推时完全一致。
- **SC-005**: 大表读取采用分批/分片流式进行，读取过程中引擎内存占用不随表总行数线性增长（无 OOM）。
- **SC-006**: 鉴权失败、表不存在、地址非法、网络不可达等错误场景，均返回可定位原因的错误信息，
  且 0 例泄露 AccessKey 明文。
