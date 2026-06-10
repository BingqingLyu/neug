# Feature: AP 链路临时图

**Input**: Design documents from `/specs/008-temp-graph/`
**Prerequisites**: plan.md (required), spec.md (required for modules)
**GitHub Feature Issue**: TBD

# Modules

- Module 1: LOAD AS 语法与临时数据接入
    - [F008-T101] Schema 层添加 temporary 标记
    - [F008-T102] PropertyGraph 层 Dump/Open/Compact 防护
    - [F008-T103] CreateTypeParam 添加 temporary 字段
    - [F008-T104] Proto 扩展 CreateVertexSchema/CreateEdgeSchema
    - [F008-T105] 执行层传递 temporary 标记
    - [F008-T106] Parser — LoadAs Statement 与 Cypher.g4 语法
    - [F008-T107] Binder — bindLoadAs 校验
    - [F008-T108] Planner — planLoadAs 与 PhysicalPlan 生成

- Module 2: 统一 Cypher 查询接口
    - [F008-T201] to_yaml() 包含 temporary labels 验证
    - [F008-T202] 混合查询端到端测试

- Module 3: 会话级生命周期与 schema 管理
    - [F008-T301] Connection::Close() 自动清理临时数据
    - [F008-T302] 生命周期端到端测试
