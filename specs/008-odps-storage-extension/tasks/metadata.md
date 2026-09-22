# Feature: ODPS/MaxCompute Table Input via Storage API Extension

**Input**: Design documents from `/specs/008-odps-storage-extension/`
**Prerequisites**: plan.md (required), spec.md (required for modules)
**GitHub Feature Issue**: [待创建] —— feature issue 尚未在 GitHub 建立（spec/plan 推送评审未执行）；创建后回填链接。

> 说明：任务按 spec 的 4 个功能模块组织（P1→P4）。Module 1 额外承载 extension 脚手架与 SDK 集成等地基工作
> （没有它 M1 无法编译/加载）。各模块末位为该模块的测试任务（对齐 spec 各模块的 Test Strategy）。
> 技术选型遵循 plan.md 的 Resolved Decisions：读路径 = Storage API（SDK `max_storage_api`，`WITH_ARROW=ON`）、
> Arrow 仅锁 extension 内 + RTLD_LOCAL 隔离、SDK 以 submodule+patch 对齐 carquet、凭据对齐 httpfs `s3_options.cc`。

# Modules

- Module 1: ODPS 表扫描数据源 (Priority: P1)
    - [F008-T101] 集成 aliyun-odps-sdk-cpp 为 third_party（submodule + Build\*AsThirdParty，WITH_ARROW=ON）
    - [F008-T102] 建立 extension/odps 骨架与注册（ODPS_SCAN 占位，可 `LOAD odps;`）
    - [F008-T103] 实现 odps_options：解析 odps:// 地址与选项 → OdpsSourceDesc
    - [F008-T104] 实现 odps_connection：封装 SDK 连接/鉴权，凭据对齐 httpfs 三件套
    - [F008-T105] 实现 sniffFunc + odps_schema_converter（基础标量类型）
    - [F008-T106] 实现 supplier/exec + odps_record_batch_supplier + odps_arrow_bridge（Arrow→DataChunk，流式分批）
    - [F008-T107] 端到端打通 LOAD FROM "odps://..." RETURN（无下推全表读）
    - [F008-T108] 错误路径与空结果处理（鉴权/表不存在/非法地址/网络不可达，0 凭据泄露）
    - [F008-T109] Module 1 单测 + 集成测

- Module 2: ODPS → 图表批量导入 (Priority: P2)
    - [F008-T201] 打通 COPY <node_table> FROM (LOAD FROM odps)（点表灌入）
    - [F008-T202] 边表灌入并映射起点/终点主键列
    - [F008-T203] 列重映射（RETURN 列序/子集与目标表不同，按名/按位对应）
    - [F008-T204] 类型契合校验与不兼容报错（指明列与类型）
    - [F008-T205] Module 2 单测 + 集成测

- Module 3: 读取下推与并行 (Priority: P3)
    - [F008-T301] 列裁剪下推（project_columns → mRequiredDataColumns）
    - [F008-T302] 分区裁剪下推（分区限定 → mRequiredPartitions）
    - [F008-T303] 谓词下推翻译（skip_rows → SDK IPredicate / SetFilterPredicate）
    - [F008-T304] 下推回退与结果等价对拍（不可下推谓词回退引擎侧过滤）
    - [F008-T305] 分片并行读取（SplitOptions 粒度可配）
    - [F008-T306] Module 3 单测 + 集成测

- Module 4: 类型覆盖与不支持项处理 (Priority: P4)
    - [F008-T401] 补全类型映射矩阵（复杂/时间/小数/二进制等）
    - [F008-T402] 精度与时区约定（含溢出/超范围策略 + 文档）
    - [F008-T403] 聚簇表 / Delta / 物化视图读取支持验证
    - [F008-T404] 不支持项显式报错（外部表/逻辑视图/JSON）
    - [F008-T405] Module 4 单测 + 集成测
