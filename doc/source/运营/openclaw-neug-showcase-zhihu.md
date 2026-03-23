# 我们用图数据库给 6,000 文件的 OpenClaw 做了次体检，发现了这些结构性隐患

> **先问大家一个问题**：你的项目有多少个函数？哪个函数改了会影响最多地方？哪些代码其实已经没人用了？
>
> 如果答不上来，这篇文章可能对你有用。

---

## 为什么需要"图视角"？

[OpenClaw](https://github.com/openclaw/openclaw) 是 GitHub 上很火的 AI 网关，18,000+ 提交、6,000+ TypeScript 文件、21,000+ 函数。上一篇我们从用户视角发现了心跳的 tokens 消耗问题，这篇换个角度——**从开发者视角，看快速迭代留下了哪些结构性债务**。

传统代码 review 和静态分析工具能发现局部问题，但有些问题藏得更深：

- 哪些函数是"定时炸弹"，出 bug 影响面最大？
- 哪些代码其实已经没人用了，却在迷惑新开发者？
- 哪个模块是系统的"枢纽"，一动全身？

这些问题的答案藏在**关系**里，不在代码行里。

---

## 怎么做的？把代码库变成一张图

我们用 NeuG 图数据库 + zvec 向量数据库，把整个代码库建成一张图：

- **函数、文件、模块** = 节点
- **调用关系、导入关系** = 边

图谱规模：

| 类型 | 数量 |
|------|------|
| 源文件 | 6,062 |
| 函数 | 21,057 |
| 调用边 | 35,761 |
| 导入边 | 25,883 |
| 模块 | 318 |

两万个函数被三万六千条调用边连在一起。有了这张图，复杂问题变成了 Cypher 查询。

整个分析流程已打包为 [CodeGraph Skill](https://github.com/alibaba/neug/tree/main/skills/codegraph) 开源发布，可直接复现。

---

## 发现一：高危函数——谁是"定时炸弹"？

### 风险值 = fan_in × fan_out

- **fan-in**：多少函数调用它（被依赖程度）
- **fan-out**：它调用多少函数（依赖复杂度）

乘积代表"爆炸半径"。

大模型编程工具统计 fan-out 很容易，但 fan-in 需要全库扫描——这正是图数据库的优势。一行代码搞定：

```python
functions = cs.hotspots(topk=30)
```

**风险程度最高的 5 个函数：**

| 函数名 | 文件名 | fan-in | fan-out | 风险程度 |
|--------|--------|--------|---------|---------|
| **startGatewayServer** | src/gateway/server.impl.ts | 10 | 103 | **1030** |
| **createConfigIO** | src/config/io.ts | 18 | 56 | **1008** |
| **runEmbeddedPiAgent** | src/agents/pi-embedded-runner/run.ts | 14 | 67 | **938** |
| **loadOpenClawPlugins** | src/plugins/loader.ts | 20 | 36 | **720** |
| **runCronIsolatedAgentTurn** | src/cron/isolated-agent/run.ts | 11 | 60 | **660** |

拿 `createConfigIO` 举例：配置解析往往没人给它写测试，但它被 18 处调用、自己又调 56 个函数——**按结构风险算，它才是全库最要命的函数之一**。

### 进一步过滤：真正的高危核心

用 Cypher 过滤出 fan-in > 10 且 fan-out > 10 的函数：

```cypher
MATCH (f:Function)-[:CALLS]->(g:Function)
WITH f, COUNT(DISTINCT g) as fan_out
MATCH (f)<-[:CALLS]-(h:Function)
WITH f, fan_out, COUNT(DISTINCT h) as fan_in
WHERE fan_in > 10 AND fan_out > 10
RETURN f.name, fan_in, fan_out
LIMIT 100
```

出乎意料，整个代码库只有 **17 个函数**满足条件，全部列在下面：

| 函数名 | 文件名 | fan-in | fan-out | 风险程度 |
|--------|--------|--------|---------|---------|
| startGatewayServer | src/gateway/server.impl.ts | 10 | 103 | 1030 |
| createConfigIO | src/config/io.ts | 18 | 56 | 1008 |
| runEmbeddedPiAgent | src/agents/pi-embedded-runner/run.ts | 14 | 67 | 938 |
| loadOpenClawPlugins | src/plugins/loader.ts | 20 | 36 | 720 |
| runCronIsolatedAgentTurn | src/cron/isolated-agent/run.ts | 11 | 60 | 660 |
| getReplyFromConfig | src/auto-reply/reply/get-reply.ts | 20 | 24 | 480 |
| runMessageAction | src/infra/outbound/message-action-runner.ts | 21 | 22 | 462 |
| loadPluginManifestRegistry | src/plugins/manifest-registry.ts | 22 | 16 | 352 |
| createOpenClawTools | src/agents/openclaw-tools.ts | 10 | 24 | 240 |
| fetchWithSsrFGuard | src/infra/net/fetch-guard.ts | 24 | 10 | 240 |
| resolveCommandSecretRefsViaGateway | src/cli/command-secret-gateway.ts | 15 | 14 | 210 |
| fetchRemoteMedia | src/media/fetch.ts | 18 | 11 | 198 |
| loadModelCatalog | src/agents/model-catalog.ts | 18 | 11 | 198 |
| resolveApiKeyForProvider | src/agents/model-auth.ts | 13 | 15 | 195 |
| start | src/gateway/client.ts | 12 | 13 | 156 |
| resolveAuthProfileOrder | src/agents/auth-profiles/order.ts | 11 | 11 | 121 |
| createClackPrompter | src/wizard/clack-prompter.ts | 10 | 11 | 110 |

这 17 个函数，是整个系统的核心高危点。

### 发现僵尸函数

把全库函数的 fan-in 分布画成直方图，会发现：**超过 20% 的函数 fan-in 为零**。

排除入口函数后，仍剩余 **2,000+ 个真正的僵尸函数**。

真实案例：`assertPublicHostname`（`src/infra/net/ssrf.ts`）
- commit `5bd550` 引入时有人用
- commit `b62355` 把功能迁移到 `resolvePinnedHostname`
- 旧函数没被删，就这么"活死"在代码库里

**危害不是碍眼**：新开发者看到它以为有用 → 基于它写代码 → debug 时钻进死胡同。

在 OpenClaw 这种日更项目里，20% 的僵尸代码率是笔不小的认知税。

---

## 发现二：高耦合模块——Bug 为什么总在同一个地方？

分析最近 20 个 bug，57 个根因候选函数中，**24 个落在 `src/agents`——占比 42%**。

用 Cypher 查一下原因：

```cypher
MATCH (m1:Module {name: 'agents'})<-[:BELONGS_TO]-(f1:File)-[:DEFINES_FUNC]->(func1:Function)
MATCH (func1)-[:CALLS]->(func2:Function)<-[:DEFINES_FUNC]-(f2:File)-[:BELONGS_TO]->(m2:Module)
WHERE m2.name <> 'agents'
RETURN m2.name, count(*) as call_count
ORDER BY call_count DESC
LIMIT 10
```

结果（a→b 表示 a 模块调用 b 模块的次数）：

```
agents <-> reply              a→b=19    b→a=117
agents <-> infra              a→b=108   b→a=15
agents <-> pi-embedded-runner a→b=19    b→a=88
agents <-> tools              a→b=38    b→a=55
agents <-> plugins            a→b=45    b→a=40
agents <-> gateway            a→b=9     b→a=60
agents <-> models             a→b=1     b→a=63
agents <-> sessions           a→b=46    b→a=5
agents <-> auth-profiles      a→b=24    b→a=12
```

**30+ 个模块与 `agents` 双向依赖**。`reply` 调用 `agents` 117 次，`pi-embedded-runner` 调用 88 次——大半个系统的业务逻辑最终都要经过这里。

`agents` 是系统的结构性枢纽，改一行代码的涟漪可传到半个项目。Bug 扎堆不是巧合，是结构使然。

---

## 图视角能看到什么

传统工具能告诉你哪行代码有语法错误、哪个函数复杂度太高。但 NeuG 能告诉你：

- 一次修改会影响多少函数
- 哪些代码其实没人用了
- 哪个模块是系统枢纽

> 当代码库规模超过人脑处理极限，结构性问题只能靠**结构查询**发现。

---

## 给 OpenClaw 开发者的建议

1. **解耦 `src/agents`**：30+ 个模块双向依赖，优先定义清晰的接口边界
2. **给高风险函数加测试**：`createConfigIO`、`startGatewayServer` 动一发牵全身，优先补覆盖
3. **清理僵尸代码**：2,000+ 个死函数，对着调用图过一遍，给后来者省大量困惑

---

**互动问题**：
- 你的项目有类似的"枢纽模块"吗？
- 你觉得 20% 的僵尸代码率算高吗？
- 你会怎么用这个思路分析自己的代码库？

欢迎评论区聊聊。

---

*分析基于 [NeuG](https://github.com/alibaba/neug) 图数据库和 CodeScope 代码分析引擎，完整索引耗时约 4 分钟。*
