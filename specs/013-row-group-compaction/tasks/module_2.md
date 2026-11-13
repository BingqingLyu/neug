# Module 2: 组内 vid 重排映射与有效行 gather

**Goal**: 对选中组生成 old_vid→new_vid 紧凑映射，把有效行 gather 到组前部、组尾留洞、base_g 不变（FR-006~009）。

**Assignee**: @me
**Label**: None（本地审阅）
**Milestone**: None
**Project**: None

## [F013-T201] 组内 vid 重排映射生成

**description**: 生成 old_vid→new_vid 紧凑映射，覆盖组内所有有效 vid，删除行不在映射中（FR-006、FR-007、FR-008）。

**details**:
- 在 row_group_compaction（M6 T601）或 VertexTable 实现映射生成。
- 扫组内 vid（base_g..base_g+G），按原顺序取有效 vid，new_vid = base_g + 递增偏移。
- 删除行不进映射（不被携带）；base_g 不变；不影响其他组。
- 例：组 0 [v0,v1(删),v2,v3] → {v0→0, v2→1, v3→2}（相对 base_0），组尾 [3,G) 留洞。
- 验收：映射覆盖所有有效 vid、删除行无映射；base_g 不变；干净组 vid 不变。

## [F013-T202] ChunkedColumn 组内行 gather

**description**: ChunkedColumn 加组内 gather 能力，按映射把有效行重排到新偏移（FR-007、FR-009）。

**details**:
- include/neug/utils/property/chunked_column.h 加组内 gather 接口（按 old→new 映射搬有效行）。
- 删除行不搬，其字节随旧数据经 GC 回收；组尾留连续空洞。
- 不动热路径 pages_[vid>>page_shift_]（gather 是维护操作，非读路径）。
- 验收：gather 后按新 vid 逐行读回值与重排前按旧 vid 一致；删除行不再可见。

## [F013-T203] M2 映射与 gather 测试

**description**: 覆盖映射生成（全有效/全删除/混合）、gather 正确性、base_g 不变、干净组隔离（FR-006~009）。

**details**:
- tests/storage/test_row_group_compaction.cc：映射生成各情形；gather 后逐行值一致；干净组隔离；重排前后磁盘占用对比。
- 验收：FR-006~009 可验证。