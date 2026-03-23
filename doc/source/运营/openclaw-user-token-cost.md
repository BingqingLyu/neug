# OpenClaw 用户必看：你的 Token 为什么莫名其妙消耗殆尽？

> *同样的对话，昨天还好好的，今天 token 突然飙升——问题不在模型，在 heartbeat。*

---

## 现象：会话历史无限增长，token 莫名飙升

如果你在使用 OpenClaw 时遇到以下情况：

- 会话历史越来越长，没有被截断
- 系统 prompt 异常膨胀（有用户反馈达到 29K 字符）
- 频繁撞上模型的 200K token 上限
- 明明配置了 `lightContext: true`，但感觉没有生效

这很可能不是你的配置问题，也不是模型问题——根源在 OpenClaw 的 **heartbeat 机制**。

---

## Heartbeat 在悄悄做什么？

OpenClaw 的 heartbeat 是一个定时器，设计上是轻量的周期性 tick。但我们用 [CodeScope](https://github.com/codescope/codescope) 对其调用链做了图分析，结果出乎意料。

通过以下 Cypher 查询，我们找出了 heartbeat 调用链中所有涉及上下文/会话加载的函数：

```cypher
// 找出 runHeartbeatOnce 调用链中涉及上下文加载的函数
MATCH (f:Function {name: 'runHeartbeatOnce'})-[:CALLS*1..3]->(t:Function)
WHERE t.name CONTAINS 'Context'
   OR t.name CONTAINS 'Workspace'
   OR t.name CONTAINS 'Session'
   OR t.name CONTAINS 'Prompt'
RETURN t.name, t.file_path
ORDER BY t.file_path
```

返回结果包含：

| 函数 | 文件 |
|------|------|
| `resolveAgentWorkspaceDir` | src/agents/ |
| `resolveHeartbeatSession` | src/infra/heartbeat-runner.ts |
| `resolveEffectiveMessagesConfig` | src/agents/identity.ts |
| `buildOutboundSessionContext` | src/infra/outbound/session-context.ts |
| `resolveHeartbeatRunPrompt` | src/infra/heartbeat-runner.ts |

**每一次 heartbeat tick，都会触发这些函数**——加载 workspace 目录（AGENTS.md、SOUL.md、TOOLS.md……）、构建完整会话上下文、生成完整 prompt。

再用一条查询确认 heartbeat-runner.ts 的依赖规模：

```cypher
// 统计 heartbeat-runner.ts 的直接导入数量
MATCH (f:Function)-[:CALLS]->(t:Function)
WHERE f.file_path CONTAINS 'heartbeat-runner'
RETURN count(DISTINCT t.file_path) AS imported_modules
```

结果：**34 个模块**。

这个数字和完整消息处理管线的依赖规模相当——heartbeat 在结构上已经和"重量级路径"没有区别。

---

## 为什么 `lightContext: true` 没有用？

你可能在 HEARTBEAT.md 里配置了 `lightContext: true`，期望 heartbeat 只加载轻量上下文。

但问题在于：**`lightContext` 是一个标志，不是一道隔离墙。**

heartbeat 的调用链在历次迭代中被逐步挂上了越来越多的依赖。到今天，它的依赖树决定了它必须加载 workspace、必须构建会话上下文、必须经过 34 个模块——这些是结构层面的路径，不是一个配置 flag 能绕过的。

给一辆满载的卡车贴一张"轻型车辆"的标签，车还是那么重。

---

## 官方也在修，但还没稳定

我们分析了最近 200 个提交的修改密度，`context-pruning` 模块（负责上下文裁剪）的修改密度高达 **0.19**——21 个函数里有 4 个在近期被反复修改。

这说明 OpenClaw 团队也意识到了上下文膨胀的问题，正在积极修复，但目前还没有找到稳定方案。

```cypher
// 查询 context-pruning 模块的近期修改情况
MATCH (f:Function)
WHERE f.file_path CONTAINS 'context-pruning'
  AND f.last_modified IS NOT NULL
RETURN f.name, f.last_modified
ORDER BY f.last_modified DESC
```

**结论：在官方给出稳定修复之前，不要依赖 `lightContext` flag 来控制 token 消耗。**

---

## 你现在能做什么：四条规避建议

**1. 把 heartbeat 当成重量级操作来对待**

不要假设 heartbeat 是轻量的。在计算 token 预算时，把每次 heartbeat tick 都纳入消耗计算，不要依赖 `lightContext: true` 来省 token。

**2. 降低 heartbeat 触发频率**

如果你的场景不强依赖心跳功能，降低触发频率是目前最直接有效的节省手段。减少 tick 次数 = 减少上下文加载次数。

**3. 手动控制会话历史长度**

不要依赖 OpenClaw 自动裁剪会话历史。在你的配置中显式设置最大历史条数，避免会话历史无限累积。

**4. 监控 token 消耗，定位异常**

在 LLM provider 侧开启 token 用量监控。如果你观察到 token 消耗有规律性的周期性峰值（而不是随对话内容增长），大概率就是 heartbeat 在定期触发全量上下文加载。

---

## 小结

| 问题 | 根因 | 当前状态 |
|------|------|----------|
| `lightContext: true` 不生效 | heartbeat 调用链结构性依赖重量级路径 | 官方修复中，尚未稳定 |
| 会话历史无限增长 | 每次 tick 都加载完整上下文，无截断 | 需用户手动配置 |
| Token 周期性飙升 | heartbeat 频率 × 每次全量加载 | 降低频率可缓解 |

如果你想自己验证这些路径，可以用 [CodeScope](https://github.com/codescope/codescope) + [NeuG](https://github.com/TuGraph-family/tugraph-db) 对 OpenClaw 代码库跑一遍图分析——上面的 Cypher 查询可以直接用。

---

*本文分析基于 CodeScope 使用 NeuG 图数据库对 OpenClaw 代码库的扫描结果。*
