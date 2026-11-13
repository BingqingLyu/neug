# Feature: 行组逻辑压实（组内 vid 重排与空洞回收）

**Input**: Design documents from `/specs/013-row-group-compaction/`
**Prerequisites**: plan.md (required), spec.md (required for modules)
**GitHub Feature Issue**: None（本地审阅，未创建远端 issue/PR）

# Modules

- Module 1: 按组空洞统计与候选选择 (优先级: P1)
    - [F013-T101] 每组空洞率计算与统计
    - [F013-T102] 空洞率阈值配置与自动候选选择
    - [F013-T103] COMPACT 命令解析与 admin 路由
    - [F013-T104] M1 统计与候选选择测试

- Module 2: 组内 vid 重排映射与有效行 gather (优先级: P1)
    - [F013-T201] 组内 vid 重排映射生成
    - [F013-T202] ChunkedColumn 组内行 gather
    - [F013-T203] M2 映射与 gather 测试

- Module 3: 四结构协同 remap (优先级: P1)
    - [F013-T301] LFIndexer keys 重编 + indices 全局重建
    - [F013-T302] VertexTimestamp 组段重编
    - [F013-T303] IndexIDAccessor 线性 remap
    - [F013-T304] M3 四结构 remap 一致性测试

- Module 4: CSR neighbor 跨组 remap (优先级: P1)
    - [F013-T401] CSR 邻居集合定位
    - [F013-T402] CSR neighbor remap 与批量去重
    - [F013-T403] M4 CSR remap 测试

- Module 5: 组尾空洞回填与每组前沿分配 (优先级: P1)
    - [F013-T501] GroupFrontier 每组前沿分配器
    - [F013-T502] insert 路径接入前沿分配 + 异 PK 解绑
    - [F013-T503] M5 前沿分配与回填测试

- Module 6: 排他窗口编排与原子发布 (优先级: P1)
    - [F013-T601] row_group_compaction 编排器
    - [F013-T602] 排他窗口集成与批量重排
    - [F013-T603] 原子发布与崩溃恢复
    - [F013-T604] M6 并发与崩溃恢复测试

# 依赖与交付说明

- **实现依赖序**：M1 统计（T101）是候选输入；M2 映射（T201）是 M3/M4 remap 的输入；M5 前沿（T501）独立可先做；
  M6 编排器（T601）协调 M2~M5，窗口集成（T602）依赖编排器；M1 命令路由（T103）触发编排器。
  建议序：T101 → T201 → T501 → (T301/T302/T303/T401/T402 并行) → T601 → T102/T103 → T602/T603 → 各测试。
- **红线**：仅逻辑压实、解耦四结构分块化（remap 整块重写）；固定组大小、base_g=g×G 均匀；不动读路径
  `pages_[vid>>page_shift_]`；物理压实（base+delta merge）不在本 spec。
- **构建验证**：`cmake --build build -j$(sysctl -n hw.physicalcpu)`（禁裸 -j）；GLOG_minloglevel=3 跑测试；make format-check。
- **选择性提交**：仅本特性触碰文件 + spec/plan/tasks；工作区无关改动（PK 预占、mvcc_insert、submodule 脏指针、未跟踪文档）不进提交。
- **性能评测**：属 implementation 后 evaluation（优先级低于实现），见 spec 测试与评测节；正确性验证（TSAN/崩溃/并发）随实现同步。