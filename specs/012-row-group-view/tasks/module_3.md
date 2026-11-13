# Module 3: vid 分解表示与跨结构映射正确性

**Goal**: 保证 vid=base_g+offset 分解在 CSR neighbor/indexer/时间戳/accessor 中映射一致，含跨组边与空洞；
非稠密有效行读取兼容。布局 B 下 vid 仍是全局行号、各结构本就按 vid 下标，**零寻址改造**，本模块以验证为主
（FR-010~015），不以"初始数值稠密"免除映射正确性。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F012-T301] 跨组边与跨结构映射端到端测试

**description**: 构造跨组边与新插点，验证 CSR neighbor 分解解析与跨结构映射一致（FR-010/012/014）。

**details**:
- 端到端测试（test_chunked_column.cc 集成段，或 tests/storage 现有图测试 fixture）：双顶点 label + unbundled 边 label（带边属性），小参数 G。
- 跨组边 v4→v1（组1→组0）：读 out_csr[4].neighbor，分解为 base_0+1，解析到组0 偏移1 顶点，其属性/时间戳/索引一致。
- 新插 v6 到组1：vid=base_1+2，keys[6]/inserted[6]/accessor[6] 与属性列第 6 行映射一致。
- 任意 vid 经 base_g+offset 分解后，属性/时间戳/索引/CSR 对端定位一致。
- **无新寻址代码**：分解是 RowGroupLayout 算术视图，各结构存储值不变、列方法不改；正确性靠测试显式保证。
- 依赖：T101、T203（及现有图存储）。

## [F012-T302] 非稠密有效行与同 PK 重插映射测试

**description**: 验证删点空洞按可见性跳过、映射仍一致；同 PK 重插复用 vid 映射一致（FR-011/013/015）。

**details**:
- 删点产生空洞：读该 vid 按 timestamp 可见性判不可见；地址空间与 base_g 不变；跨结构映射一致。
- 非稠密有效行：地址空间连续均匀、有效行非全局稠密（含组尾空洞模型），读取不要求全局稠密。
- 同 PK 重插复用原 vid：各结构按该 vid 映射仍一致（基线复用行为不被破坏）。
- 初始稠密态：base_g=g×G、组满数值稠密，现有物理下标仍工作；分段分解与映射按分段语义显式验证（不以稠密免除）。
- 依赖：T301。

## [F012-T303] 读路径回归与现有测试无回退

**description**: 验证引入视图后投影/排序/过滤查询结果不变，现有 storage/transaction 测试无回退（FR-008、SC-003）。

**details**:
- 引入前后对同一图跑投影/排序/过滤查询，结果逐行一致。
- 跑现有 chunk_foundation_test、chunk_fault_recovery_test、storage_test、test_vertex_table、edge_table_test 无回退。
- 端到端吞吐无可测回退（可选微基准对比）。
- 依赖：T203、T301、T302。