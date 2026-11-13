# Feature Specification: 行组逻辑压实（组内 vid 重排与空洞回收）

**Feature Branch**: `013-row-group-compaction`
**Created**: 2026-11-13
**Status**: Draft
**Input**: User description: "基于 row group 的 compaction（012 的下一个 spec）；仅逻辑压实：组内 vid 重排、删除空洞回收、四结构 remap、组尾空洞回填复用；解耦四结构分块化（整块重写即可）；物理压实（base+delta 按组 merge）另立后续 spec"

> 唯一权威设计来源：仓库根下「分块COW行组最终形态设计说明.md」（下称**最终形态文档**，尤其 §3、§4.3、§5.6、§7.2）
> 与 spec `012-row-group-view`（row group 结构地基）。讨论过程中的历史文档（碎片整理说明 / 完整设计文档 / 总纲的旧 P4
> 章节）含过期信息（全局重排 / 依赖四结构先分块化），不作为依据；本 spec 已按调研结论修正这些前提（见关键决策）。

## 背景与范围

`012-row-group-view` 已交付 row group 结构地基：布局 B 逻辑分组、`base_g=g×rows_per_group` 均匀纯算术、
组边界持久化、表级组枚举与按组脏统计。但**删除顶点留下的 vid 空洞仍不回收**——`VertexTable::Compact()` 只压
时间戳（`v_ts_->Compact()`），vid 空洞回收挂 TODO；`LFIndexer` 无 remap 接口；`ApplyVidRemapping` 纯属设计、零实现。

删点不断累积 vid 空洞：属性列把行范围内每个槽都物理写入，删除行的无效字节随 Dump 落盘，整对象 GC 回收不掉
（行槽仍被目录引用）；只有重排 vid、把有效行紧凑、删除行不再被携带，才能回收这些字节。旧方案是**全局重排**
（所有点 vid 全变、四结构全量 remap、成本正比全表），row group 把它降为**组内局部重排**（只重排脏组、干净组不动）。

> **四结构**（vid 重排协同口径）：属性列、vid/indexer+时间戳（合为一组，重排时按同一组元数据一起 remap）、CSR、索引。
> 区别于数据组织口径的“五类结构”（属性列、vid/indexer、时间戳、索引、CSR）。

**核心模型（沿用 012）**：固定组大小、`base_g=g×G` 均匀、组内 gather 有效行到组前部、**组尾留空洞**、干净组不动。
局部压实必须放弃有效行**全局稠密**（否则组 g 少一行会迫使后续所有组前移、退化成全局重排），但保留 vid 地址空间
**连续均匀**（`group_id=vid/G` 纯算术不变）。组尾空洞可被后续 insert 顺序回填（每组追加前沿）。

**关键调研结论（修正旧 P4 前提）**：旧 P4 把"vid 重排"依赖"四结构先分块化（P3）"。本 spec 论证该依赖对**局部**重排
不成立——remap 是 vid 引用的**逻辑重写**，整块持久化即可完成（indexer keys 组内 gather + indices 全局重建哈希 +
时间戳整块重建 + CSR 整文件 remap）；分块化是**物理 I/O 优化**（增量 dump、按组 merge），不是 remap 正确性前提。
旧依赖源于旧方案是**全局**重排（必须重写一切）；row group **局部**重排只改脏组引用，整块重写 indexer/时间戳可接受。
故**逻辑压实解耦分块化、独立落地**；物理压实（base+delta 按组 merge）依赖分块化，另立后续 spec。

**本特性范围内（仅逻辑压实）**：

- 按组空洞统计与候选选择（空洞率阈值触发 + 手动）；
- 组内 vid 重排（old_vid→new_vid 映射生成、有效行 gather 到组前部、组尾留洞、base_g 不变）；
- 四结构协同 remap（属性列行 gather、indexer keys 重编 + indices 全局重建、时间戳组段重编、索引 accessor 线性 remap）；
- CSR neighbor 跨组 remap（波及面=重排组邻居集合，批量重排去重邻居）；
- 组尾空洞回填复用（每组追加前沿分配、异 PK 复用解绑旧 oid）；
- 排他窗口编排与原子发布（复用全局 checkpoint 维护窗口、批量重排摊薄、崩溃恢复）。

**本特性范围外**（各有归属）：

- **物理压实**（base+delta 按组对齐 merge、列级/组级 gather 收敛跨对象数）：依赖四结构分块化，另立后续 spec
  （011 框架的组化）。本特性 MUST NOT 声称收敛跨对象数或做 base+delta merge。
- **四结构分块化迁移**（CSR/indexer keys/时间戳迁 chunked 底座，旧 P3）：物理压实的前置，非本特性依赖。
- **索引 index_id tombstone 回收**（FTS/vector 引擎 blob 重编 index_id + 重写）：与 vid 重排解耦（R14），属索引内部事项。
- **时间线重置**（uint32 时间戳耗尽、`VertexTimestamp::Compact` 基线归零）：独立操作（R16），可搭同一排他窗口但触发/内容互不绑定。
- **变长属性（VARCHAR/LIST/ARRAY）的空洞回收**：依赖 `010-var-len-chunked-cow`，另行展开。
- **bundled 边属性**：无独立属性列，跟随 CSR。

## 关键决策与假设

- **固定组大小 + 均匀基址 + 组尾空洞**（沿用 012 §7.2.3）：`rows_per_group` 不变、`base_g=g×G` 不变、组内 gather
  有效行回收空洞、尾部留洞、干净组不动。**被拒**：可变组大小（后续组基址前移→所有后续 vid 全变、remap 波及全局）。
- **逻辑压实解耦分块化**（本 spec 调研结论，修正旧 P4）：remap 用整块重写 indexer/时间戳/CSR 即可，不依赖四结构
  先分块化。**被拒**：沿用旧 P4"先分块化再重排"依赖（那是全局重排的 I/O 优化前提，对局部重排不成立）。
- **复用全局排他窗口、批量重排**：vid 重排改现有点 vid，须在排他窗口内回收旧快照后执行（删除行不被携带的安全前提）；
  复用既有 `WithCheckpointMaintenance` 全局排他单点发布，批量重排多个脏组摊薄静默成本，不引入组级窗口、不做第二发布者。
  **被拒**：COW 快照内并发重排（CSR neighbor 跨组扫描的原子发布代价过大）；独立组级排他窗口（与单发布者机制冲突）。
- **vid/gid 不承诺跨重排稳定**（R15）：vid/gid 是内部坐标，重排改 vid 故 gid 变；稳定标识用 PRIMARY KEY（oid 是属性
  数据、重排不改）；bolt element_id 生成逻辑不变、不引入独立稳定 id 列。
- **时间线重置解耦**（R16）：与 vid 重排是两个独立操作，可搭同一排他窗口摊薄静默成本，但触发/内容/窗口分别定义、互不绑定。
- **indices 全局重建**：indexer indices 是 `hash(oid)→vid` 开放寻址哈希，槽位由 oid 哈希决定（不按 vid）。vid 重排后
  按全部有效 vid 重建哈希槽（扫描 keys 取 oid、重新插入）。**被拒**：原地逐槽更新（开放寻址删除会断线性探测链，正确性复杂）。
  注：indices 重建是全局操作（成本正比有效顶点总数），是组内 vid 重排里唯一不可局部化的 indexer 成本。
- **按组批量重排、全 label 协同**：CSR neighbor remap 天然跨 label（一条边连两个可能不同 label 的点），故重排以组为单位、
  跨 label 协同；批量重排多个脏组、合并去重邻居集合，避免逐组重复改写共享邻居。
- **默认触发阈值保守**：空洞率阈值默认保守（收益纯空间、读不受益、正确性不依赖），支持手动触发；insert/update-only
  负载永不触发重排（只有 delete 产生空洞）。

## Functional Modules *(mandatory)*

### Module 1: 按组空洞统计与候选选择 (Priority: P1)

**Purpose**: 维护每组的空洞率，提供两种触发模式选出重排候选：**自动**（按可配置空洞率阈值选组）与**手动**
（显式 COMPACT 命令即立即重排，不过阈值）。干净组不动。这是所有重排动作的输入。

**Why this priority**: 后续重排都消费这里的候选集；统计本身可独立验证（删若干点后核对各组空洞率与暴力扫描时间戳一致）。

**Independent Test**: 对某组删若干点，验证系统报告该组空洞率与暴力扫描时间戳的结果一致；自动模式下空洞率低于阈值的组
不被选中；手动命令立即重排有空洞的组（不过阈值）；数据库重开后统计可由时间戳重建。

**Key Components**:

1. **每组有效行数与空洞率**: 组内有效行数 / 组容量，空洞率 = 1 − 有效行数/组容量。
2. **自动触发（阈值配置）**: 空洞率阈值暴露为用户配置（默认保守）；后台调度选空洞率超阈值的组。
3. **手动触发（显式命令）**: Cypher 管理命令 `COMPACT;` / `COMPACT TABLE <label>;`，立即重排 scope 内有空洞的组，
   不过阈值（命令本身即 force 语义，无独立 FORCE 关键字）；走与 CHECKPOINT 相同的 admin 路径。
4. **跨重排保留**: 统计与候选跨 checkpoint 与重开保留（可由时间戳重建）。
5. **跨组引用量估计**: 估计组的邻居波及面（度数），供调度优先重排低波及组。

**Functional Requirements**:

1. **FR-001**: 系统 MUST 维护每组的有效行数与空洞率，作为重排触发指标。
2. **FR-002**: 系统 MUST 提供可配置的空洞率阈值；自动压实 MUST 仅选空洞率超阈值的组。
3. **FR-003**: 系统 MUST 提供手动压实命令 `COMPACT;` 与 `COMPACT TABLE <label>;`，立即重排 scope 内有空洞（空洞率 > 0）
   的组，MUST NOT 受自动阈值限制；命令 MUST 走 admin 路径，无独立 FORCE 关键字。
4. **FR-004**: 空洞率统计 MUST 跨 checkpoint 与数据库重开保留（可由时间戳重建）。
5. **FR-005**: 自动压实 MUST NOT 对空洞率低于阈值的组触发重排；insert/update-only 负载 MUST NOT 触发自动重排。

**Acceptance Scenarios**:

1. **Given** 某组删了若干点，**When** 查询该组空洞率，**Then** 与暴力扫描时间戳的结果一致。
2. **Given** 一个空洞率低于阈值的组，**When** 自动调度考虑重排，**Then** 不选中该组。
3. **Given** 一个空洞率低于阈值但 > 0 的组，**When** 用户执行 `COMPACT TABLE <label>;`，**Then** 该组被立即重排（不过阈值）。
4. **Given** 数据库重开，**When** 恢复完成，**Then** 各组空洞率与重开前一致。

**Test Strategy**:

- **Unit Tests**: 空洞率计算；自动阈值选择；手动命令立即重排（不过阈值）；统计重建。
- **Integration Tests**: 多轮删除后统计与暴力扫描对拍；重开后统计一致性；COMPACT 命令端到端（admin 路径）。

---

### Module 2: 组内 vid 重排映射与有效行 gather (Priority: P1)

**Purpose**: 对选中的组生成 old_vid→new_vid 紧凑映射，把有效行 gather 到组前部、组尾留洞，base_g 不变。这是空洞回收
的核心动作，也是四结构 remap 的输入。

**Why this priority**: 这是本特性对用户可见收益（空洞回收、磁盘占用停止随删除增长）的直接来源。依赖 Module 1 的候选集，
单独可验证（构造组内空洞，重排后核对有效行紧凑、映射正确）。

**Independent Test**: 构造组内若干空洞，触发重排，验证有效行紧凑到组前部、组尾留洞、base_g 不变、old→new 映射覆盖所有
有效 vid 且删除行不在映射中。

**Key Components**:

1. **映射生成**: 扫描组内时间戳算出有效 vid 集合，按原顺序生成 old_vid→new_vid 紧凑映射（new_vid = base_g + 新偏移）。
2. **有效行 gather**: 按映射把有效行搬到新偏移；删除行拿不到 new_vid、不被携带，其字节随旧数据经 GC 回收。
3. **组尾留洞**: 重排后有效行紧凑到 [base_g, base_g+有效行数)，组尾 [base_g+有效行数, base_g+G) 留连续空洞。
4. **base_g 不变**: 组大小固定，重排不改 base_g、不影响其他组。

**Functional Requirements**:

1. **FR-006**: 系统 MUST 对选中组生成 old_vid→new_vid 映射，覆盖该组所有有效 vid，删除行 MUST NOT 出现在映射中。
2. **FR-007**: 重排 MUST 把有效行 gather 到组前部、组尾留连续空洞，base_g MUST 不变。
3. **FR-008**: 重排 MUST NOT 改变其他未选中组的任何 vid 或数据。
4. **FR-009**: 重排后按新 vid 逐行读回的有效行值 MUST 与重排前按旧 vid 读回的值一致。

**Acceptance Scenarios**:

1. **Given** 组 0 有 v0、v1(空洞)、v2、v3，**When** 重排，**Then** v2→新偏移1、v3→新偏移2，v1 空洞回收，base_0 不变，组 0 有效行 3 个。
2. **Given** 组 0 重排，**When** 检查组 1，**Then** 组 1 所有 vid 与数据不变。
3. **Given** 重排完成，**When** 按新 vid 读回有效行，**Then** 值与重排前按旧 vid 读回一致。

**Test Strategy**:

- **Unit Tests**: 映射生成（含全有效/全删除/混合）；gather 正确性；base_g 不变。
- **Integration Tests**: 组内空洞重排后逐行值一致；干净组隔离；重排前后磁盘占用对比。

---

### Module 3: 四结构协同 remap (Priority: P1)

**Purpose**: 按同一 old_vid→new_vid 映射，同步重排四结构（属性列、vid/indexer+时间戳、CSR、索引），保证一致性。
vid 是四结构共享的隐式坐标（vid=N 即各结构第 N 槽），重排必须四处同步，否则下标错位。

**Why this priority**: 这是重排正确性的核心。任何一处漏 remap 都会导致 PK 查找、可见性、索引定位错误。与 Module 2 同为 P1。

**Independent Test**: 重排后验证 PK 查找（oid→vid→属性）、可见性（时间戳）、索引定位（vid→index_id）全部正确；
删除行不再可见；引擎 blob 不动。

**Key Components**:

1. **属性列 gather**: 按映射把有效行重排到新偏移（组内局部）。
2. **indexer keys 重编**: keys[vid]=oid 按新 vid 重排（组内 gather）。
3. **indexer indices 全局重建**: 按全部有效 vid 重建 hash(oid)→vid 哈希槽（全局，槽位由 oid 哈希决定）。
4. **时间戳组段重编**: inserted_vertices_ 按新偏移重排（组段局部）；removed_vertices_ 稀疏集相应更新。
5. **索引 accessor 线性 remap**: vid_to_index_id_ 按新 vid 重编（组内局部）；引擎 blob 按 index_id 寻址不动。

**Functional Requirements**:

1. **FR-010**: 重排 MUST 按同一映射同步 remap 四结构（属性列、vid/indexer+时间戳、CSR、索引 accessor），保证按新 vid 一致。
2. **FR-011**: indexer indices MUST 按全部有效 vid 重建哈希槽，重建后所有有效 PK 的 oid→vid 查找 MUST 正确。
3. **FR-012**: 索引引擎 blob MUST NOT 因 vid 重排被重写（按 index_id 寻址）；accessor remap MUST 为线性。
4. **FR-013**: 重排后删除行 MUST 不再可见，其属性字节 MUST 随旧数据经 GC 回收。
5. **FR-014**: 重排 MUST NOT 回收索引 index_id tombstone（与 vid 重排解耦）。

**Acceptance Scenarios**:

1. **Given** 组 0 重排，**When** 用某有效 PK 查找，**Then** oid→新 vid→属性值正确。
2. **Given** 组 0 重排，**When** 查删除行的 PK，**Then** 不可见。
3. **Given** 组 0 重排，**When** 检查索引引擎 blob，**Then** blob 未变，accessor 按新 vid 映射正确。

**Test Strategy**:

- **Unit Tests**: 四结构 remap 一致性；indices 重建后 PK 查找；accessor 线性 remap。
- **Integration Tests**: 重排后 PK/可见性/索引端到端正确；删除行回收；引擎 blob 不动。

---

### Module 4: CSR neighbor 跨组 remap (Priority: P1)

**Purpose**: 组内 vid 重排改变该组顶点 vid，指向这些顶点的 neighbor 字段散布在其他组的邻接链表里，必须 remap。
波及面限于重排组的邻居集合（经该组自己的 out/in 邻接定位），不必全图扫描。这是 CSR 独有、属性列没有的跨组成本。

**Why this priority**: 漏 remap 会导致图遍历指向错误顶点。与 Module 3 同为 P1，是重排正确性的必要部分。

**Independent Test**: 构造跨组边，重排某组后验证所有指向该组顶点的 neighbor 字段已 remap 到新 vid；非邻居组不受影响；
批量重排多个组时共享邻居只改写一次。

**Key Components**:

1. **邻居集合定位**: 经重排组 g 的 out_csr/in_csr 定位 neighbor∈g 的对端（即 g 的 in/out 邻居），波及面被 g 度数封顶。
2. **neighbor remap**: 把邻居邻接表里指向 g 旧 vid 的 neighbor 字段改为新 vid。
3. **批量去重**: 批量重排多个脏组时合并去重邻居集合，避免重复改写同一邻居邻接表。
4. **整块重写**: CSR 当前整文件持久化，remap 后整文件重写（不依赖分块化）。

**Functional Requirements**:

1. **FR-015**: 系统 MUST remap 所有指向重排组顶点的 CSR neighbor 字段到新 vid，波及面 MUST 限于重排组邻居集合。
2. **FR-016**: 系统 MUST NOT 为 remap 全图扫描所有邻接记录；邻居集合 MUST 经重排组自己的 out/in 邻接定位。
3. **FR-017**: 批量重排多个组时，系统 MUST 合并去重邻居集合，同一邻居邻接表 MUST NOT 被重复改写。
4. **FR-018**: remap 后图遍历 MUST 指向正确顶点，跨组边与跨 label 边 MUST 正确。

**Acceptance Scenarios**:

1. **Given** 组 1 的 v4→组 0 的 v1，组 0 重排 v1→新 vid，**When** 读 out_csr[4]，**Then** neighbor 已 remap 到 v1 的新 vid。
2. **Given** 组 0 重排，**When** 检查非邻居组的邻接表，**Then** 不受影响。
3. **Given** 批量重排组 0 与组 1（共享邻居 v5），**When** remap，**Then** v5 邻接表只改写一次。

**Test Strategy**:

- **Unit Tests**: 邻居集合定位；neighbor remap；批量去重。
- **Integration Tests**: 跨组/跨 label 边重排后遍历正确；非邻居组隔离；批量重排去重。

---

### Module 5: 组尾空洞回填与每组前沿分配 (Priority: P1)

**Purpose**: 重排后组尾留连续空洞，后续 insert 应能回填这些空洞（否则空洞永不复用、空间浪费）。把 vid 分配从全局计数器
改为每组追加前沿（vid=base_g+frontier_g），压实后前沿回退到有效行数，insert 顺序回填组尾。异 PK 复用空洞时须解绑旧 oid。

**Why this priority**: 这是空洞回收闭环的关键——重排产生组尾洞，回填复用洞。没有回填，重排回收的空间很快又被新插入
浪费。依赖 Module 2 的组尾洞，单独可验证。

**Independent Test**: 重排某组后插入新点，验证新点 vid 落进组尾空洞（前沿回填）；异 PK 复用后旧 PK 查找不再指向新点；
初始稠密态插入行为与基线一致。

**Key Components**:

1. **每组追加前沿**: 每组维护 frontier_g，vid=base_g+frontier_g；初始稠密态前沿=组内行数，顺序 append 活跃组尾部。
2. **前沿回退**: 压实后前沿回退到有效行数，后续 insert 从前沿顺序回填组尾连续空洞。
3. **异 PK 解绑**: 复用组尾 vid 给新 PK 时，MUST 解除旧 oid→vid 的 indices 绑定，避免旧 PK 错误指向新点。
4. **同 PK 重插**: 同 PK 重插复用原 vid 属基线行为，不变。

**Functional Requirements**:

1. **FR-019**: 系统 MUST 把 vid 分配改为每组追加前沿（vid=base_g+frontier_g），初始稠密态行为与基线全局计数器一致。
2. **FR-020**: 压实后系统 MUST 把组前沿回退到有效行数，后续 insert MUST 能顺序回填组尾连续空洞。
3. **FR-021**: 异 PK 复用组尾 vid 时，系统 MUST 解除旧 oid→vid 的 indices 绑定，旧 PK 查找 MUST NOT 指向新点。
4. **FR-022**: 回填 MUST 保持组内偏移单调（连续顺序回填），MUST NOT 抢压实前的散布异 PK 空槽。
5. **FR-023**: 回填 MUST NOT 破坏 MVCC 可见性（新点按新时间戳判定，旧快照不读新点）。

**Acceptance Scenarios**:

1. **Given** 组 0 压实后有效行 3 个（前沿=3），**When** 插入新点，**Then** 新点 vid=base_0+3（回填组尾洞）。
2. **Given** 组尾 vid 原属 PK=A（已删），**When** 新 PK=B 复用该 vid，**Then** PK=A 查找不再指向该 vid，PK=B 正确。
3. **Given** 初始稠密态，**When** 插入新点，**Then** vid 顺序 append，与基线一致。

**Test Strategy**:

- **Unit Tests**: 每组前沿分配；前沿回退；异 PK 解绑；同 PK 重插。
- **Integration Tests**: 压实→回填闭环；旧 PK 不指向新点；MVCC 可见性；初始稠密态基线一致。

---

### Module 6: 排他窗口编排与原子发布 (Priority: P1)

**Purpose**: vid 重排改现有点 vid，必须在排他窗口内回收旧快照后执行（删除行不被携带的安全前提），并原子发布。
复用既有全局 checkpoint 维护窗口（WithCheckpointMaintenance），批量重排摊薄静默成本，compaction 不做第二发布者。

**Why this priority**: 这是重排能安全上线的前提。没有正确的窗口与发布，重排会破坏快照隔离、被长事务饿死、或与前台
checkpoint 竞争。与 Module 2/3/4 同为 P1。

**Independent Test**: 前台持续写入 + 周期 checkpoint 的同时触发重排，验证重排在排他窗口内执行、旧快照已回收、发布原子、
崩溃回退一致、无 TSAN race。

**Key Components**:

1. **全局排他窗口**: 复用 WithCheckpointMaintenance（drain readers + update exclusive），窗口内回收旧快照后执行 remap。
2. **批量重排**: 一次窗口内重排多个脏组，摊薄静默成本；合并去重 CSR 邻居。
3. **单点发布**: remap 产物挂 checkpoint 单点发布（原子切 manifest + reopen），compaction 不做第二发布者。
4. **最老快照封顶**: 长事务 pin 旧快照时重排无净收益，调度跳过/延后而非硬等。
5. **崩溃恢复**: 发布前崩溃回退到重排前状态，孤儿产物经整对象 GC 清理。

**Functional Requirements**:

1. **FR-024**: vid 重排 MUST 在全局排他窗口内、回收旧快照后执行；MUST NOT 在仍有旧快照 pin 时丢弃删除行。
2. **FR-025**: 重排产物 MUST 通过 checkpoint 单点原子发布（manifest + reopen）；compaction MUST NOT 成为第二发布者。
3. **FR-026**: 调度 MUST 把最老 live 快照年龄作为一等输入；长事务 pin 使重排无净收益时 MUST 跳过/延后而非硬等。
4. **FR-027**: 发布前崩溃 MUST 回退到重排前的一致状态，孤儿产物 MUST 被 GC 清理。
5. **FR-028**: 重排 MUST 与时间线重置解耦（可搭同一窗口，触发/内容互不绑定）。

**Acceptance Scenarios**:

1. **Given** 前台持续写入并周期 checkpoint，**When** 触发重排，**Then** 重排在排他窗口内执行、发布原子、前台读写在窗口外不阻塞。
2. **Given** 长事务 pin 旧快照，**When** 调度考虑重排，**Then** 判定无净收益并跳过/延后。
3. **Given** 重排产物写出但未发布，**When** 崩溃重启，**Then** 回退到重排前状态、孤儿被 GC 清理。

**Test Strategy**:

- **Unit Tests**: 窗口前置条件（旧快照回收）；单点发布；最老快照判定；崩溃回退。
- **Integration Tests**: 前台写 + 周期 checkpoint × 重排并发压测（TSAN）；长事务 pin 下跳过；多注入点崩溃恢复。

---

### Edge Cases

- 高连接度邻居：remap 成本=每个邻居整条邻接表重写，高连接度邻居贵；调度优先重排低波及组（跨组引用量少）。
- 长事务 pin 最老快照：重排持续无净收益，MUST 跳过而非累积待办；调度记录跳过原因。
- 重排期间前台 insert：排他窗口阻塞写，窗口外 insert 正常；回填与重排不并发（重排改前沿，回填消费前沿）。
- 组全删除（有效行 0）：重排后组前沿=0，整组 vid 号段留空（组槽保留、base 不变），后续 insert 可回填。
- 同 PK 重插与异 PK 回填交互：同 PK 重插复用原 vid（基线）；异 PK 回填须解绑旧 oid；两者路径不同。
- indices 全局重建成本：成本正比有效顶点总数，是组内重排里唯一不可局部化的 indexer 成本；大表重排须计入。
- 跨 label 边：CSR neighbor remap 天然跨 label，重排以组为单位跨 label 协同。
- 初始稠密态：无空洞时不触发重排；每组前沿分配退化为基线全局计数器行为。
- 重排与时间线重置搭车：可同一窗口，但触发独立（时间戳耗尽周期性、空洞只在异 PK delete 产生）。
- 变长属性列：本特性不回收变长属性空洞（依赖 010），重排时变长列按 vid 跟随 gather 但空洞字节回收另行处理。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 删点累积空洞后启用重排，组内有效行紧凑到组前部、组尾留洞，磁盘占用停止随删除轮次单调增长
  （删除行字节经 GC 回收）。
- **SC-002**: 重排只改脏组 vid，干净组 vid 与数据 MUST 完全不变；重排成本 MUST 正比脏组数据量而非全表。
- **SC-003**: 重排后四结构（属性/PK 查找/可见性/索引）按新 vid 逐行读回 MUST 完全一致；删除行 MUST 不再可见。
- **SC-004**: CSR neighbor remap 后图遍历 MUST 指向正确顶点（含跨组/跨 label 边）；非邻居组 MUST 不受影响。
- **SC-005**: 组尾回填后新点 vid 落进组尾洞；异 PK 复用后旧 PK 查找 MUST NOT 指向新点；MVCC 可见性 MUST 正确。
- **SC-006**: 重排在排他窗口内执行、单点原子发布；前台读写在窗口外 MUST 不阻塞；发布窗口时长 MUST 与重排数据量解耦
  （批量摊薄）。
- **SC-007**: 重排任意注入点崩溃，重启后数据库 MUST 回退一致状态、无损坏、孤儿被 GC 清理。
- **SC-008**: 前台持续写 + 周期 checkpoint × 重排的并发压测在 TSAN 下 MUST 零 race（重排新代码路径）。
- **SC-009**: indices 全局重建后所有有效 PK 查找 MUST 正确；索引引擎 blob MUST 不被重写。

## 测试与评测（实现后 evaluation，优先级低于实现）

本节定义实现完成后的验证与评测要求。**正确性验证随实现同步落地**（各模块 Test Strategy + SC-001~009）；
**性能基准属 implementation 之后的 evaluation 阶段，优先级低于实现**。性能评测参考既有「分块 COW 基准评测计划.md」
的指标维度（§3.2）与 workload 轴（§4.1），但 reorder 场景需扩展（见下）。

### 正确性保证（并发与复杂场景）

- **TSAN 零 race**：前台持续写 + 周期 checkpoint × 后台重排的并发压测，重排新代码路径 MUST 零 race（SC-008）。
- **MVCC 隔离**：读者持旧快照 × 重排/回填，旧快照读旧值正确、新点按新时间戳可见；排他窗口回收旧快照后才丢弃删除行（FR-024）。
- **并发 insert × 重排**：排他窗口阻塞写、窗口外 insert 正常；回填与重排不并发（重排改前沿、回填消费前沿）；并发 insert 不产生重复 vid / PK 冲突。
- **PK 唯一性**：异 PK 回填解绑旧 oid 后，旧 PK 查找不指向新点、新 PK 唯一（FR-021）；同 PK 重插复用原 vid 不破坏唯一性。
- **崩溃恢复**：重排任意注入点崩溃，重启回退一致状态、孤儿被 GC 清理（SC-007，复用磁盘故障注入 harness）。
- **四结构一致**：重排后属性/PK 查找/可见性/索引/CSR 遍历逐行一致（SC-003/004/009）。

### 性能对比（vs main，防回退）

**基线 = main**（双构建真 A/B，同「分块 COW 基准评测计划.md」§6 方法）。核心问题：013 是否引入性能回退。

- **未触发压实的常规负载**（insert/update/read，无 delete 空洞）：吞吐与 p50/p99 延迟 MUST NOT 相对 main 回退。
  重点关注 vid 分配从全局计数器改为每组前沿（FR-019）的 insert 路径开销——初始稠密态 MUST 与基线一致。
- **触发压实**：空间回收收益（ckpt_bytes 停增并回落）vs remap 代价（CPU/IO）+ 排他窗口停顿（ckpt_pause）；
  收益与代价同列验收（双向 delta，不只报喜）。
- **读路径**：重排后点读/扫描读延迟不劣化（组内有效行连续）；全局稠密非目标，组尾洞的读代价如实记录。

**指标维度**（参考基准计划 §3.2）：copied_bytes / data_bytes / dir_bytes / 写放大 / 跨对象数 / object_count /
ckpt_bytes / 吞吐 / 延迟 p50/p99/max / ckpt_pause / 峰值 RSS。reorder 重点关注 ckpt_bytes（空间回收）、
ckpt_pause（窗口停顿）、remap CPU/IO、吞吐/延迟（防回退）。

**workload 轴**（参考基准计划 §4.1）：局部性（单点/Zipf/随机/区间）、操作类型（insert/update/delete/读/OLTP）、
规模（行数/op 数/checkpoint 间隔）、churn。reorder 需扩展：

- **churn 轴补“异 PK delete”**：既有 churn 轴只有 append-only / 同 PK 重插（复用原 vid、空洞自然填回，基线行为）；
  013 回收的是**异 PK delete 留下的空洞**（删 A 插 B，A 的 vid 空洞留存），须新增此 workload（基准计划 W10/W14 相关）。
- **W14 改为局部重排口径**：既有 W14“vid 重排（第二套）”旧口径是全局重排；013 是组内局部重排，评测须测
  局部 remap 代价（正比脏组数据量 + 邻居波及面）而非全表，并验证干净组零开销。
- **新增 reorder 专属 workload**：异 PK delete churn（空洞累积→重排回收）、局部组内重排代价（不同空洞率/邻居度数）、
  重排后读正确性与延迟、组尾回填闭环（重排→insert 回填→再重排）。

**方法**：双构建 A/B（main 基线 vs 013 分支），固定编译开关与随机种子、每 workload 多轮取均值（同基准计划 §6）；
原始输出移入「分块 COW 基准与故障测试记录.md」，spec 只留结论。

**优先级**：本节性能基准低于实现优先级，属 implementation 完成后的 evaluation 阶段；正确性验证（TSAN/崩溃/并发）
随实现同步落地，不延后。