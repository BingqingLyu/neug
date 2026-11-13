# Feature Specification: 行组（Row Group）逻辑视图结构

**Feature Branch**: `012-row-group-view`
**Created**: 2026-11-12
**Status**: Draft
**Input**: User description: "引入 RowGroup 作为表级逻辑视图（布局 B）：跨列统一行组边界、base_g=g×G 均匀纯算术；vid 地址空间连续均匀但允许组尾空洞（非全局稠密），组尾空洞允许后续 insert 复用；删点空洞只在所在组内 reorder（compaction 动作放下一个 spec）；所有按 vid 下标的结构映射保证正确"

> 唯一权威设计来源：仓库根下「分块COW行组最终形态设计说明.md」（下称**最终形态文档**）。本 spec 以该文档为准
> （尤其 §1、§2.3、§3.1、§7.2.3）；其中 §7.2.1 关于"不复用删点空槽 / 全局计数器顺序分配"的旧结论，已按本次
> 决策修订为"组尾空洞可复用 / 每组追加前沿"（见该节）。讨论过程中的历史文档含过期信息，不作为依据。

## 背景与范围

NeuG 的定长属性列已完成分块 COW 改造（`009-fixed-len-chunked-cow`）：覆盖写只复制被改到的脏子页，不再整列拷贝。
但留下两个缺口：

1. **chunked 优化只针对列**，缺乏一个跨所有列、按统一行数对齐的**行级管理单位**来协调"整行"的更新；
2. **删点产生行空洞**，要把空洞压实，目前必须**全局重排所有点的 vid**。

RowGroup 正是为解决这两点而引入：

1. **行级视角**：跨列统一的行粒度结构，协调一个点整行（所有列）的更新；
2. **局部压实**：删点空洞只在所在组内 reorder vid，**不波及全局**。

**核心推演（本 spec 的模型依据）**：局部压实的硬约束是"压实组 g 的空洞时，其他组的 vid 不变"。
若要求 vid **全局稠密**（有效行恰好 [0,N)、无空洞），组 g 少一个有效行会迫使后续所有组的有效行前移 →
所有后续 vid 全变 → 退化成全局重排，目标 2 失败。因此**局部压实必须允许空洞存在（放弃全局稠密）**；
但这只需要"固定组大小 + 组内 gather 有效行到前部 + 组尾留洞"，**不需要物理分段、不需要可变基址、不需要组目录**。
vid 地址空间可以保持**连续均匀**（base_g=g×G、group_id=vid/G 纯算术）。

**本特性采用的模型（布局 B 逻辑分组）**：

- **RowGroup 是表级第一公民逻辑视图**：一个组对应一张表（点集或边属性集）的一段统一行范围，逻辑上"包含"该范围内
  的各列（chunked 列）。类比关系：
  > ChunkedColumn 之于 ColumnBase（一列的切分实现） ≈ RowGroup 之于点集表（整个点集的切分视图）。
- **物理布局 B 不变**：各列保持扁平 `pages_` 存储、子页级 COW 不变。RowGroup 通过行范围*引用*现有扁平列来"包含"
  它们，**不物理拥有各列的分段拷贝**。组边界是 vid 算术边界、`pages_` 扁平数组无 per-group 物理分配目录
  （§2.3、§3.1、§7.2.1）；列内布局独立于组划分，列方法一行不改（§2.3）。
- **均匀基址纯算术**：base_g = g×rows_per_group 固定均匀（§7.2.3），`group_id = vid / rows_per_group`、
  `offset = vid % rows_per_group` 纯算术成立，组间无间隙。
- **连续 ≠ 稠密**：vid **地址空间连续均匀**（组间无间隙），但**有效行非全局稠密**——组内压实后有效行紧凑到组前部、
  **组尾留空洞**，base_g 不动、干净组不动。这正是 compaction 能按组局部做、不必全局重排 vid 的前提。
- **组尾空洞可复用**（本次新增决策）：后续 insert 可复用组尾空洞——分配模型为**每组追加前沿**
  （`vid = base_g + frontier_g`，压实后前沿回退到有效行数、insert 从前沿顺序回填组尾）。vid **不再全局插入单调**
  （新点可能落进较早组的组尾），但地址空间仍均匀、纯算术定位不变、MVCC 可见性按 timestamp 判定不受影响。

本特性交付**结构地基**：RowGroup 逻辑视图 + 统一组边界与组算术 + vid 分解与跨结构映射正确性 + 组边界持久化 +
组级脏度统计，并保证与"组尾空洞可复用"模型兼容。

**本特性范围内**：统一行组边界与组算术；RowGroup 逻辑视图与表级组枚举；vid 分解表示与跨结构映射正确性；
非稠密有效行（空洞）的读取兼容；组尾空洞可复用的模型与结构兼容（每组前沿抽象）；组边界持久化与重开恢复；
组级脏度统计（只统计、不整理）；图层端到端集成（VertexTable/EdgeTable 组枚举委托）；上述能力的读取兼容与正确性。

**本特性范围外**（各有归属）：

- **基于 row group 的 compaction 动作**：组内 gather（产生组尾空洞）、CSR neighbor 跨组 remap、indices 哈希重建、
  **insert 回填组尾的具体机制与组选择策略/并发协调**——作为**下一个 spec**。列级 compaction 见
  `011-prop-column-compaction`（列级 gather/base+delta，不含行组协调）；本特性只交付结构与映射，不交付任何整理/重排/回填动作。
- **CSR / indexer / 时间戳 / 索引的按组组织、按组落盘、组段 COW**：阶段二协调（最终形态文档 §3.2–§3.5、§6）。
  本特性保证这些结构的 vid 表示与映射正确，但不改其物理组织与落盘粒度。
- **变长属性（VARCHAR/LIST/ARRAY）的分块**：见 `010-var-len-chunked-cow`。
- **bundled 边**（属性内嵌邻接数组、无独立列）：无列可归组，跟随 CSR，属后续。

## 关键决策与假设（依最终形态文档 §7.2，§7.2.1 已修订）

- **逻辑视图而非物理容器（布局 B）**：RowGroup 通过行范围引用现有扁平列，"包含各列"是逻辑引用而非物理拥有；
  表仍持有扁平列，组边界是 vid 算术边界、`pages_` 扁平无组目录，不碰物理预留（§7.2.1）。
  **被拒**：物理组容器 / 表级 segment tree / 可变基址（DuckDB 式）——那是"组可合并 / 组大小可变"才需要的更重方案，
  对"固定组大小 + 组内压实"这个目标超出所需，且会推翻已落地的 P0 扁平寻址。
- **均匀基址纯算术**：base_g = g×rows_per_group 固定均匀（§7.2.3），纯算术定位。**被拒**：可变组大小
  （回收后后续组基址前移会让所有后续组 vid 全变、破坏算术定位、remap 波及全局）。
- **连续 ≠ 稠密**：vid 地址空间连续均匀；有效行非全局稠密；组尾空洞允许（压实后）。**局部压实必须放弃全局稠密**，
  但可保留地址空间连续均匀。这是本 spec 区别于"vid 全局稠密不变"错误表述的关键。
- **组尾空洞可复用**（本次新增决策，修订 §7.2.1）：压实后的**组尾连续空洞**允许后续 insert 顺序回填；
  分配模型为**每组追加前沿** `base_g + frontier_g`；vid 非全局插入单调但地址空间均匀。
  **与被拒方案区分**：文档拒绝的是"散布异 PK 删点空槽 free-list 抢洞"（压实前抢散布单洞，破坏单调 vid 算术、并发竞态）；
  组尾复用是**压实后的连续顺序回填**，组内偏移仍单调，二者不同。组选择策略与并发协调随 compaction spec 定义。
- **vid 分配**：每组前沿模型；初始稠密态（无空洞）行为与基线一致；复用机制随 compaction（下一个 spec）。
  同 PK 重插复用原 vid 属基线行为。
- **跨结构映射**：CSR neighbor、indexer keys[vid]/indices[hash(oid)]=vid、时间戳 inserted[vid]、索引
  accessor vid_to_index_id_[vid] 统一用 vid（分解为 base_g+offset），映射一致；本特性不做这些结构的按组落盘/组段 COW（阶段二）。
- **默认每组行数 262144（2^18）**：各列每 chunk 行数为 2 的幂且 ≤262144（bool 宽度 1 达上限），"组=整数个逻辑
  chunk"要求每组行数是各列 chunk 行数的公倍数，最小通用值即 262144（§2.1）。未来可配置，本特性先用默认常量。
- **组级统计只统计不整理**：脏度统计是组结构可用性与后续组级 compaction 的挂载点，本特性 MUST NOT 触发任何
  搬运/重写/回收/重排/回填。

## Functional Modules *(mandatory)*

### Module 1: 统一行组边界与组算术 (Priority: P1)

**Purpose**: 为一张表的所有列确立统一的每组行数，base_g = g×rows_per_group 固定均匀，组边界由纯算术唯一确定，
一个组恒等于整数个逻辑 chunk。这是 RowGroup 结构与 vid 分解的地基。

**Why this priority**: 没有跨列统一的组边界与均匀基址，"组包含各列"和纯算术 vid 定位都无从谈起。它单独即可验证，
是其余模块的前提。

**Independent Test**: 构造含不同宽度列（bool、INT64、宽类型）的表，验证所有列在任意行号上归属同一组；验证
base_g=g×G、group_id=vid/G、offset=vid%G、组起止行算术在组首/组尾/跨组相邻行处正确；验证每组行数是各列 chunk 行数的公倍数。

**Key Components**:

1. **统一每组行数**: 一张表共享一个每组行数常量，所有列据此对齐组边界。
2. **均匀组基址**: base_g = g×rows_per_group 固定，组间无间隙，保住纯算术定位。
3. **组算术**: 行号→组号、组号→行范围、行号→组内偏移的纯算术映射，无需维护边界表/组目录。
4. **整除约束**: 每组行数 MUST 是各列每 chunk 行数的公倍数（同为 2 的幂天然成立），组=整数个逻辑 chunk。
5. **稳定边界**: 表增长只新增组，base_g 固定、不重划已有组。

**Functional Requirements**:

1. **FR-001**: 系统 MUST 为一张表的所有列采用统一的每组行数，使同一行号在所有列中归属同一个组。
2. **FR-002**: 系统 MUST 保证每组行数是各列每 chunk 行数的公倍数（一个组等于整数个逻辑 chunk）。
3. **FR-003**: 系统 MUST 以 base_g = g×rows_per_group 为组基址，支持 group_id=vid/rows_per_group、offset=vid%rows_per_group、组号→行范围的纯算术定位，边界行正确。
4. **FR-004**: 系统 MUST 在表增长时只新增组、base_g 固定、不重新划分已有组的行范围。

**Acceptance Scenarios**:

1. **Given** 一张含 bool 列与 INT64 列的表（两列每 chunk 行数不同），**When** 取任意行号，**Then** 该行在两列中归属同一组号。
2. **Given** 每组行数 G，**When** 查询行号 G-1、G、G+1，**Then** 组号分别为 0、1、1，组 1 基址为 G、组内偏移分别为 0、1。
3. **Given** 表已有若干组，**When** 追加新行跨组边界，**Then** 新增一个组、base_g=g×G，已有组行范围不变。

**Test Strategy**:

- **Unit Tests**: 组算术边界（行 0、组边界±1、尾行）；整除约束校验（非法每组行数被拒绝）；不同列宽跨列对齐。
- **Integration Tests**: 多列表上逐行核对组归属跨列一致。

---

### Module 2: RowGroup 逻辑视图与表级组枚举 (Priority: P1)

**Purpose**: 把一张表表达为一组有序的 RowGroup，每个 RowGroup 携带组基址 base_g、对应一段连续行范围、逻辑上包含
该范围内该表的所有列，支持按组访问各列、按全局 vid 定位所属组，并暴露每组的有效行数/追加前沿抽象（与组尾空洞可复用兼容）。
这是本特性的核心结构交付。

**Why this priority**: 这是用户诉求的本体——"row group 作为逻辑视图、里面包含各列"。建立在 Module 1 的组边界之上，
单独即可验证，交付"点集被切分为含列的组"这一结构能力。

**Independent Test**: 对已填充的多列表枚举全部 RowGroup，验证每组携带正确 base_g、覆盖正确行范围且包含全部列；
通过任一组访问其范围内任一列任一行，结果与按全局 vid 访问一致；按 vid 定位组正确；尾组正确；每组前沿/有效行数抽象可读取。

**Key Components**:

1. **RowGroup 逻辑视图**: 携带组基址 base_g，对应行范围 [base_g, base_g+rows_per_group)，逻辑包含该范围内所有列；
   通过行范围引用现有扁平列，不拥有分段拷贝。
2. **表级组枚举**: 表可遍历为有序 RowGroup 序列，含最后一个不足整组的尾组。
3. **按 vid 定位组**: 给定全局 vid，定位所属 RowGroup 与组内偏移（纯算术）。
4. **组内按列访问**: 通过 RowGroup 以组内偏移访问指定列，落回现有列的全局 vid 读取（vid=base_g+offset）。
5. **每组前沿/有效行数抽象**: 组暴露追加前沿与有效行数视图，初始稠密态前沿=有效行数=组内行数；
   该抽象与"压实后前沿回退、insert 回填组尾"兼容（机制属下一个 spec）。

**Functional Requirements**:

1. **FR-005**: 系统 MUST 把一张表表达为一组有序的 RowGroup，每个 RowGroup 携带组基址、对应一段连续行范围并逻辑上包含该范围内该表的所有列。
2. **FR-006**: 系统 MUST 支持通过 RowGroup 访问其行范围内任一列的数据，结果与直接按全局 vid 访问该列一致。
3. **FR-007**: 系统 MUST 支持按全局 vid 定位所属 RowGroup 与组内偏移，并支持枚举表的全部 RowGroup（含不足整组的尾组）。
4. **FR-008**: 系统 MUST 保证引入 RowGroup 视图后，现有读取路径（投影/排序/过滤的连续区间读取）结果与行为不变。
5. **FR-009**: 系统 MUST 为每个 RowGroup 暴露追加前沿/有效行数抽象，初始稠密态与基线行为一致，且结构上与组尾空洞可复用兼容（不引入物理 per-group 分配单元）。

**Acceptance Scenarios**:

1. **Given** 一张 N 行多列表，**When** 枚举 RowGroup，**Then** 各组 base_g=g×G、行范围首尾相接覆盖 [0, N)，末组为尾组，每组含全部列。
2. **Given** 某 RowGroup 及其范围内一行，**When** 通过该组以偏移访问某列，**Then** 读回值与按 vid=base_g+offset 直接访问一致。
3. **Given** 一个全局 vid，**When** 定位其所属组，**Then** 返回组基址与偏移满足 vid=base_g+offset，与组算术一致。

**Test Strategy**:

- **Unit Tests**: 组枚举覆盖性与尾组；按 vid 定位组与偏移；组内按列访问与全局访问对拍；空表/单行表；前沿/有效行数抽象。
- **Integration Tests**: 点表与 unbundled 边属性表上枚举组并按组读取；引入视图前后查询结果与读取路径回归。

---

### Module 3: vid 分解表示与跨结构映射正确性 (Priority: P1)

**Purpose**: 让 vid 在所有按 vid 下标的结构中按 base_g + 组内偏移分解定位，并保证映射正确；vid 地址空间连续均匀，
有效行允许非全局稠密（空洞，含组尾空洞），读取按可见性正确跳过空洞。尤其 CSR 邻接记录里的邻接点 id 按 vid 分解后
必须正确解析到对端顶点。这是结构地基的正确性核心。

**Why this priority**: vid 表示不一致会让 CSR 遍历、索引定位、可见性判定全部出错。它依赖 Module 1 的组算术，
但独立可验证（构造跨组边核对 neighbor 解析、跨结构映射对拍），是"结构真实可用"的必要条件，而非 compaction 才做。

**Independent Test**: 构造跨组边（如组1 的 v4 → 组0 的 v1），验证 out_csr[4] 的 neighbor 存的 vid 分解为 base_0+1
后正确解析到组0 偏移1 的顶点，其属性/时间戳/索引一致；验证新插点的 vid 在 keys/inserted/accessor 中映射一致；
验证删点产生空洞后读取按可见性跳过、跨结构映射仍一致；验证任意 vid 经 base_g+offset 分解后跨结构定位一致。

**Key Components**:

1. **vid 分解表示**: vid=base_g+offset，地址空间连续均匀，group_id/offset 纯算术（§7.2.1、§7.2.3）；
   分配采用每组前沿模型，初始稠密态与基线一致。
2. **非稠密有效行**: 有效行允许有空洞（删点散布洞、压实后组尾洞）；地址空间不变，读取按 timestamp 可见性跳过空洞。
3. **CSR neighbor**: 邻接记录 {neighbor, timestamp, data} 的 neighbor 字段存 vid；读取时按 vid→(group,offset)
   解析到对端顶点（§3.2）。跨组边两端各属其组。
4. **indexer**: keys[vid]=oid 按 vid 下标；indices[hash(oid)]=vid 存 vid（§3.3）。
5. **时间戳**: inserted_vertices_[vid] 按 vid 下标（§3.4）。
6. **索引 accessor**: vid_to_index_id_[vid] 按 vid 下标（§3.5）。
7. **映射一致性**: 同一 vid 经分解定位到的属性、时间戳、索引、CSR 对端 MUST 一致。

**Functional Requirements**:

1. **FR-010**: 系统 MUST 把 vid 按 base_g + 组内偏移分解定位，地址空间连续均匀，group_id/offset 由纯算术得出。
2. **FR-011**: 系统 MUST 支持有效行非全局稠密（含组尾空洞）：地址空间与组基址不变，读取按可见性正确跳过空洞，不要求全局稠密。
3. **FR-012**: CSR 邻接记录的 neighbor 字段 MUST 存 vid，读取时 MUST 按分解正确解析到对端顶点（跨组边亦正确）。
4. **FR-013**: indexer keys/indices、时间戳 inserted、索引 accessor MUST 按 vid 下标/存值，映射与属性列一致。
5. **FR-014**: 系统 MUST 保证任意 vid 经 base_g+offset 分解后，定位到的属性、时间戳、索引与 CSR 对端一致（跨结构映射正确）。
6. **FR-015**: 初始 base_g=g×G、组满时 vid 数值稠密，现有物理下标 MUST 仍工作；分段分解与跨结构映射 MUST 按分段语义显式保证正确，MUST NOT 以"数值稠密"免除映射正确性。

**Acceptance Scenarios**:

1. **Given** 跨组边 v4→v1（组1→组0），**When** 读 out_csr[4] 的 neighbor，**Then** neighbor 的 vid 分解为 base_0+1，解析到组0 偏移1 的顶点，其属性/时间戳/索引一致。
2. **Given** 新插 v6 到组1，**When** 分配 vid，**Then** vid=base_1+2，keys[6]/inserted[6]/accessor[6] 与属性列第 6 行映射一致。
3. **Given** 删点产生空洞，**When** 读取该 vid，**Then** 按可见性判定为不可见，跨结构映射仍一致，地址空间与组基址不变。
4. **Given** 同 PK 重插复用原 vid，**When** 复用发生，**Then** 各结构按该 vid 的映射仍一致（基线复用行为不被破坏）。

**Test Strategy**:

- **Unit Tests**: vid 分解算术；CSR neighbor 分解解析（含跨组）；keys/indices/inserted/accessor 跨结构映射对拍；空洞读取跳过。
- **Integration Tests**: 跨组边遍历正确；插入/删除后跨结构映射一致；同 PK 重插映射一致；引入前后查询结果回归。

---

### Module 4: 组边界持久化与重开恢复 (Priority: P1)

**Purpose**: 把每组行数持久化进列目录，使 checkpoint 后重开恢复一致组划分；兼容未记录组信息的旧目录。
没有持久化，组结构只在内存存在、重开即丢失。

**Why this priority**: 组结构要跨 checkpoint/重开稳定存在就必须持久化边界。依赖 Module 1，单独可验证。

**Independent Test**: 对表 checkpoint 后重开，验证组划分一致；打开未记录组信息的旧目录，验证按默认每组行数回退、
读取正确、再 checkpoint 后组信息被写入。

**Key Components**:

1. **目录记录每组行数**: 列目录持久化每组行数（沿用 v1 版本化目录格式的保留标记位，不消耗新版本号）。
2. **旧目录回退**: 未记录组信息的旧目录重开按默认每组行数回退，回退值满足"是各列 chunk 行数的公倍数"。
3. **校验与回收兼容**: 组信息加入不破坏目录校验和与 GC 的对象引用解析；旧版本目录仍可被新代码读取。
4. **基址算术可得**: base_g=g×G 由组号算术得出，本特性无需持久化组基址；v2 row_start 与稀疏页数组随 compaction
   （组内重排产生组尾空洞）引入（最终形态文档 §2.2）。

**Functional Requirements**:

1. **FR-016**: 系统 MUST 在列目录中持久化该列的每组行数，使 checkpoint 后重开恢复一致的组划分。
2. **FR-017**: 系统 MUST 兼容未记录组信息的旧目录：重开按默认每组行数回退，回退值满足"是各列 chunk 行数的公倍数"。
3. **FR-018**: 系统 MUST 保证组信息持久化不破坏现有目录校验与 GC 的对象引用解析，旧版本目录仍可被新代码读取。

**Acceptance Scenarios**:

1. **Given** 一张已划分组的表，**When** checkpoint 后重开，**Then** 组划分（组数、各组行范围、base_g）与重开前一致。
2. **Given** 未记录组信息的旧目录，**When** 用新代码打开，**Then** 按默认每组行数回退、逐行读取正确、跨结构映射一致。
3. **Given** 旧目录回退后再 checkpoint，**When** 重开，**Then** 组信息已持久化、组划分稳定。
4. **Given** 含组信息的新目录，**When** GC 解析，**Then** 内部数据对象引用被正确纳入保留集判定。

**Test Strategy**:

- **Unit Tests**: 目录编解码保留每组行数；旧目录回退；非法每组行数拒绝；校验和覆盖新字段。
- **Integration Tests**: checkpoint→重开组划分一致；旧 checkpoint 打开→回退→再 checkpoint；GC 与故障注入回归。

---

### Module 5: 组级脏度统计（compaction 挂载点，只统计不整理） (Priority: P2)

**Purpose**: 支持按组报告脏页/脏字节统计，作为组结构可用性证明与后续组级 compaction 的输入挂载点。只统计、不整理。

**Why this priority**: 统计让"组"具备可观测的行粒度状态，是后续组级 compaction（下一个 spec）的输入；不是结构成立的
必要条件，且明确不含整理动作，故列 P2。

**Independent Test**: 只改某组内一行，验证脏统计只计入该组、其他组为零；验证按需计算、写路径无新增组级计数开销；
验证口径与现有列级脏统计一致。

**Key Components**:

1. **按组聚合脏页/脏字节**: 按需单遍扫描列的页，按组归属聚合。
2. **组隔离**: 一行改动只计入所属组。
3. **零热路径开销**: 不在写路径维护组级计数，仅维护窗口按需计算。
4. **只统计不整理**: MUST NOT 触发搬运/重写/回收/重排/回填。

**Functional Requirements**:

1. **FR-019**: 系统 MUST 支持按组报告脏页/脏字节统计，口径与现有列级脏统计一致。
2. **FR-020**: 系统 MUST 保证组级统计按需计算，MUST NOT 在写入热路径维护组级计数。
3. **FR-021**: 系统 MUST 保证一行改动只计入其所属组的脏统计，不污染其他组。
4. **FR-022**: 系统 MUST NOT 在本特性内触发任何数据整理/搬运/重写/回收/重排/回填（compaction 属后续 spec）。

**Acceptance Scenarios**:

1. **Given** 一张多组表，**When** 只改组 1 内一行，**Then** 组 1 脏统计非零、其余组为零。
2. **Given** 写入热路径，**When** 对比引入前后写入吞吐，**Then** 无可测回退。
3. **Given** 任意写入与 checkpoint 序列，**When** 运行本特性，**Then** 不发生任何搬运/重写/回收/重排/回填。

**Test Strategy**:

- **Unit Tests**: 组级脏统计组隔离；按需计算；与列级脏统计口径对拍；Clone/快照共享段统计口径。
- **Integration Tests**: 多轮散布更新后按组统计与暴力解析对拍；写入吞吐基准无回退。

---

### Module 6: 图层端到端集成 (Priority: P1)

**Purpose**: 把 RowGroup 视图与组枚举接到图层：VertexTable/EdgeTable 暴露组访问器，经 PropertyGraph 现有
get_vertex_table/get_edge_table 即可按 label 端到端访问组结构，无需新增图层存储。

**Why this priority**: 让组结构在图层真实可用（不止于 Table 层），是"行级视角"落地到点/边表的最后一环。
依赖 M1/M2，单独可验证。

**Independent Test**: 构造含 chunked 属性的点 label 与 unbundled 边 label，经 VertexTable/EdgeTable 组访问器
枚举组、按 vid/eid 定位组、组内按列读取与全局一致；bundled 边（无属性表）group_count=0。

**Key Components**:

1. **VertexTable 组委托**: row_group_layout/group_count/group_at/group_of 委托属性表 table_。
2. **EdgeTable 组委托**: 同名访问器作用于 unbundled 边属性表；bundled（table_ 为空）group_count=0、
   row_group_layout 返回默认、group_at/group_of 抛异常。
3. **PropertyGraph 透传**: 经现有 get_vertex_table(label)/get_edge_table(src,dst,edge) 访问，无需新增成员。

**Functional Requirements**:

1. **FR-023**: VertexTable MUST 暴露 row_group_layout/group_count/group_at/group_of，委托其属性表，结果与直接访问该表一致。
2. **FR-024**: EdgeTable MUST 暴露同名组访问器作用于 unbundled 边属性表；bundled（无属性表）时 group_count MUST 为 0、group_at/group_of MUST 抛异常。
3. **FR-025**: 系统 MUST 支持经 PropertyGraph 现有 get_vertex_table/get_edge_table 按 label 访问组结构，不新增图层存储。

**Acceptance Scenarios**:

1. **Given** 含 chunked 属性的点 label，**When** 经 VertexTable 枚举组并按 vid 定位，**Then** 与直接访问属性表一致。
2. **Given** unbundled 边 label，**When** 经 EdgeTable 枚举组并按 eid 定位，**Then** 组内按列读取与全局一致。
3. **Given** bundled 边 label（无属性表），**When** 调 group_count，**Then** 为 0；group_at/group_of 抛异常。

**Test Strategy**:

- **Unit Tests**: VertexTable/EdgeTable 组委托与表级一致；bundled 空表保护。
- **Integration Tests**: 经 PropertyGraph get_vertex_table/get_edge_table 按 label 端到端访问组。

---

### Edge Cases

- 尾组不足整组（行数非每组行数整数倍）：组枚举、按 vid 定位、组级统计是否正确。
- 空表 / 单行 / 行数不足一个 chunk：组划分（0 组或 1 组）与统计是否正确。
- 不同列宽（bool vs INT64 vs 宽类型）导致各列每 chunk 行数不同：组边界是否仍跨列对齐。
- 旧目录（无组信息）重开：回退默认每组行数，读取、跨结构映射与再 checkpoint 是否一致。
- 自定义大 chunk（每 chunk 行数 > 默认每组行数）的旧目录：回退值是否取"默认与 chunk 行数的较大者"以满足整除。
- 表增长跨组：新组追加、base_g 固定，旧组边界与已有组统计是否不变。
- 组边界正好落在物理子页 / chunk 边界：定位与统计是否正确。
- Clone / 快照共享下列的组统计：是否按本列视图聚合、不重复计共享段。
- 边属性表（unbundled）与点表共用同一组机制：eid 组划分是否与 vid 同构、边属性列目录是否记录每组行数。
- 每组行数非法（非 2 的幂 / 小于 chunk 行数 / 不被整除）：是否被拒绝而非产生错位组。
- **非稠密有效行（空洞）**：删点散布洞、压实后组尾洞，读取按可见性跳过、地址空间与组基址不变、跨结构映射一致。
- **组尾空洞可复用兼容性**：每组前沿抽象在初始稠密态与基线一致；结构上不引入物理 per-group 分配单元，为后续回填留接口。
- **跨组边 neighbor 解析**：src/dst 分属不同组时，CSR neighbor 的 vid 分解是否正确解析到对端（含级联删边弄脏对端组）。
- **同 PK 重插复用 vid**：复用原 vid 时各结构按该 vid 的映射是否仍一致。
- **初始稠密 vid 与分段分解一致性**：base_g=g×G、组满时数值稠密，分段分解与映射是否仍按分段语义正确。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 对含不同宽度列的表，所有列在同一行号上归属同一组，组边界跨列完全对齐（可由组算术逐行验证）。
- **SC-002**: 表可枚举为有序 RowGroup，每组携带 base_g、逻辑包含全部列；通过组访问任一列任一行，结果与全局 vid 直接访问逐行一致。
- **SC-003**: vid=base_g+offset 分解在 CSR neighbor、indexer、时间戳、索引 accessor 中映射正确；跨组边 neighbor 正确解析到对端顶点；现有投影/排序/过滤查询结果与引入前逐行一致，端到端吞吐无可测回退。
- **SC-004**: checkpoint 后重开组划分与重开前一致；未记录组信息的旧目录重开按默认回退、逐行读取与跨结构映射正确。
- **SC-005**: 组级脏统计按组隔离（改一行只脏其所属组）、按需计算；写入热路径相对引入前无新增开销（基准对比无回退）。
- **SC-006**: 本特性不触发任何数据整理/搬运/重写/回收/重排/回填；compaction 相关行为零引入（由后续 spec 交付）。
- **SC-007**: 点表与 unbundled 边属性表均能划分组、按组访问与统计；边属性列目录持久化每组行数、重开一致。
- **SC-008**: 任意 vid 经 base_g+offset 分解后跨结构映射一致（属性/时间戳/索引/CSR 对端），含跨组边、同 PK 重插与空洞场景。
- **SC-009**: 有效行非全局稠密（含组尾空洞）时，地址空间与组基址不变、读取按可见性正确跳过空洞；每组前沿抽象与组尾空洞可复用模型兼容。
- **SC-010**: 点 label 与 unbundled 边 label 经 VertexTable/EdgeTable（及 PropertyGraph getter）可枚举组、按 vid/eid 定位、组内读取与全局一致；bundled 边 group_count=0。