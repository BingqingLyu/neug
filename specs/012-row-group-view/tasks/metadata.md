# Feature: 行组（Row Group）逻辑视图结构

**Input**: Design documents from `/specs/012-row-group-view/`
**Prerequisites**: plan.md (required), spec.md (required for modules)
**GitHub Feature Issue**: None（本地审阅，未创建远端 issue/PR）

# Modules

- Module 1: 统一行组边界与组算术 (优先级: P1)
    - [F012-T101] RowGroupLayout 布局契约与默认常量
    - [F012-T102] ChunkedColumn 承载布局与组访问器
    - [F012-T103] M1 组算术与校验单元测试

- Module 2: RowGroup 逻辑视图与表级组枚举 (优先级: P1)
    - [F012-T201] ColumnBase 组大小访问器
    - [F012-T202] RowGroup 表级视图头文件
    - [F012-T203] Table 组枚举 API
    - [F012-T204] M2 视图与组枚举测试

- Module 3: vid 分解表示与跨结构映射正确性 (优先级: P1)
    - [F012-T301] 跨组边与跨结构映射端到端测试
    - [F012-T302] 非稠密有效行与同 PK 重插映射测试
    - [F012-T303] 读路径回归与现有测试无回退

- Module 4: 组边界持久化与重开恢复 (优先级: P1)
    - [F012-T401] ChunkDirectory 字段与 flags 编解码
    - [F012-T402] ChunkedColumn Open 回退 / Clone / Builder
    - [F012-T403] M4 目录契约与重开测试

- Module 5: 组级脏度统计（compaction 挂载点，只统计不整理） (优先级: P2)
    - [F012-T501] GroupDirtyStats 与懒式组级统计
    - [F012-T502] M5 统计测试与写吞吐回归

- Module 6: 图层端到端集成 (优先级: P1)
    - [F012-T601] VertexTable 组枚举委托
    - [F012-T602] EdgeTable 组枚举委托（bundled 保护）
    - [F012-T603] 图层集成测试

# 依赖与交付说明

- **实现依赖序**：M1（T101→T102）是地基；M4（T401 需 T101 的 Validate，T402 需 T102/T401）、
  M2（T201 需 T102，T203 需 T201/T202）、M5（T501 需 T102）可并行推进；M3 为验证模块，依赖 M1/M2 落地后跑。
- **零寻址改造红线**：布局 B，列热路径 `pages_[row>>page_shift_]` 一行不改；不引入物理组容器/组目录/segment tree。
- **范围红线**：组尾回填机制、组选择策略、并发协调、CSR remap、indices 重建均属下一个 compaction spec；本特性零整理动作。
- **构建验证**：`cmake --build build -j$(sysctl -n hw.ncpu) --target chunk_foundation_test chunk_fault_recovery_test`，
  `GLOG_minloglevel=3` 跑二进制；全量构建 + `make format-check`。禁裸 -j。
- **选择性提交**：仅本特性 9 个文件（2 新头 + 7 改）+ spec/plan/tasks；工作区无关改动（PK 预占、mvcc_insert、
  tests CMakeLists PK 注册段、submodule 脏指针、未跟踪文档）一律不进提交；`git add` 逐文件 + `git diff --cached` 核对。