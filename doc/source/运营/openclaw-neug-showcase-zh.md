# 用 NeuG 透视 OpenClaw：6,000 文件代码库的结构性体检报告

> *当代码库膨胀到 2 万个函数、3 万条调用关系，肉眼 review 已经不够用了。我们用 NeuG 图数据库给 OpenClaw 做了一次全身扫描，发现了一些结构性隐患。*

---

## 背景：为什么需要图视角

[OpenClaw](https://github.com/openclaw/openclaw) 是一个热门 AI 网关项目，18,000+ 提交、6,000+ TypeScript 文件、21,000+ 函数，项目迭代极快。上一篇推文中，我们从用户视角，发现心跳带来的tokens消耗远超我们的想象。在这篇博文中，我们想进一步深入，从开发者的视角，发现了一些在快速迭代中留下的结构性债务——高风险函数埋藏在深处、模块之间耦合缠绕。

传统的代码审查和静态分析工具能发现一些问题，但无法回答**结构性问题**：

- 哪些函数是"定时炸弹"，出 bug 影响面最大？
- 哪些代码其实已经没人用了，却在迷惑开发者？
- 哪个模块是系统的"枢纽"，一动全身？

这些问题藏在**关系**里，不在代码行里。

---

## 方法论：把代码库变成一张图

我们的解决方案是用基于NeuG图数据库和zvec向量数据库，构建代码知识图谱：

- **节点**：函数、类、模块、文件 等
- **边**：调用关系、导入关系、修改历史 等
  
  以下是部分节点类型/边类型数量概览：

| **类型**   | **数量**   |
| ------ | ------ |
| 源文件 | 6,062  |
| 函数   | 21,057 |
| 调用边 | 35,761 |
| 导入边 | 25,883 |
| 类     | 233    |
| 模块   | 318    |

可以看到，两万个函数被三万六千条调用边连在一起。有了这张图，复杂问题变成了 Cypher 查询。

此外，我们也把整个基于[NeuG](https://github.com/alibaba/neug)和[zvec](https://github.com/alibaba/zvec)的探索分析的过程打包成了[CodeGraph Skill](https://github.com/alibaba/neug/tree/main/skills/codegraph)（我们在上篇中做了介绍，这里不再赘述），发布在NeuG中。你可以通过CodeGraph复现本文提到的分析过程。

---

## 发现一：高风险函数

### 发现高危函数

在静态代码健康度分析中，有一项重要指标是分析函数的调用复杂度。一般来说，函数的被调用次数越高，说明函数的复用程度越高，也意味着这部分代码的影响范围也会更广；而在一个函数中调用其他函数越多，说明这个函数的依赖也越多，其中任何一个部分的修改都可能导致这个函数出现bug。

如果代码的调用次数和/或被调用次数过高，说明这个代码涉及的内容过多，非常容易“牵一发而动全身”。因此，我们给出函数的**风险值定义**，来衡量函数的"爆炸半径"——即该函数出 bug 的影响面：

`风险值 =  fan_in × fan_out`

* ​**fan-in**​：多少函数调用它（被依赖程度）
* ​**fan-out**​：它调用多少函数（依赖复杂度）

大模型编程工具对于fan-out的检测非常容易，只需要统计函数中调用外部函数的数量即可；但对于fan-in却难以统计，涉及到对于全库的扫描。而通过将代码库建模成图，我们可以非常快捷地定位这些fan-in/fan-out较高的热点函数：也就是统计各个函数的出边入边数量。**在CodeScope中，我们也直接提供了一个预设函数hotspots(topk)，用于快速统计fan-in和fan-out并计算风险程度。**

```
functions = cs.hotspots(topk=30)
```

我们截取风险程度最高的五个函数：

| **函数名**                   | **文件名**                               | **fan-in** | **fan-out** | **风险程度** |
| ------------------------------ | ------------------------------------------ | ------------ | ------------- | -------------- |
| **startGatewayServer**       | **src/gateway/server.impl.ts**           | **10**     | **103**     | **1030**     |
| **createConfigIO**           | **src/config/io.ts**                     | **18**     | **56**      | **1008**     |
| **runEmbeddedPiAgent**       | **src/agents/pi-embedded-runner/run.ts** | **14**     | **67**      | **938**     |
| **loadOpenClawPlugins**      | **src/plugins/loader.ts**                | **20**     | **36**      | **720**      |
| **runCronIsolatedAgentTurn** | **src/cron/isolated-agent/run.ts**       | **11**     | **60**      | **660**      |

可以看到，这些函数的fan-in和fan-out都较高，说明它们是重要的**核心功能代码**，同时也是**高危函数**。例如`startGatewayServer` 用于启动整个openclaw服务，顺序调用各个子模块，`createConfigIO` 用于生成各项配置信息，`runEmbeddedPiAgent` 则是排队执行一次嵌入式Pi Agent。这些代码处于调用链路的核心位置，任何改动都可能“牵一发而动全身”。有意思的是，拿这里的`createConfigIO` 函数举例子，配置解析这种东西往往不会被当作核心逻辑来写测试，但有18个地方在调用它，它自己又调用 56 个函数——动它一下，整个配置加载链条都可能出问题。所以但按结构风险算，它才是全库最要紧的函数之一。

为了进一步细化查询，我们手动编写cypher来定制查询时的过滤需求。例如，我们想排除一些fan-in过高而fan-out较低的底层函数，可以设置过滤条件为fan-in和fan-out均大于10：

```
MATCH (f:Function)-[:CALLS]->(g:Function)
    WITH f, COUNT(DISTINCT g) as fan_out
    MATCH (f)<-[:CALLS]-(h:Function)
    WITH f, fan_out, COUNT(DISTINCT h) as fan_in
    WHERE fan_in > 10 AND fan_out > 10
    RETURN f.name, fan_in, fan_out
    LIMIT 100
```

出乎我们的意料，结果竟然只有17个函数，这些函数都是值得我们注意的重要且高危的函数：

| **函数名** | **文件名** | **fan-in** | **fan-out** | **风险程度** |
| --------------------------------------- | ----------------------------------------------- | ---------- | ----------- | ------------ |
| **startGatewayServer** | **src/gateway/server.impl.ts** | **10** | **103** | **1030** |
| **createConfigIO** | **src/config/io.ts** | **18** | **56** | **1008** |
| **runEmbeddedPiAgent** | **src/agents/pi-embedded-runner/run.ts** | **14** | **67** | **938** |
| **loadOpenClawPlugins** | **src/plugins/loader.ts** | **20** | **36** | **720** |
| **runCronIsolatedAgentTurn** | **src/cron/isolated-agent/run.ts** | **11** | **60** | **660** |
| **getReplyFromConfig** | **src/auto-reply/reply/get-reply.ts** | **20** | **24** | **480** |
| **runMessageAction** | **src/infra/outbound/message-action-runner.ts** | **21** | **22** | **462** |
| **loadPluginManifestRegistry** | **src/plugins/manifest-registry.ts** | **22** | **16** | **352** |
| **createOpenClawTools** | **src/agents/openclaw-tools.ts** | **10** | **24** | **240** |
| **fetchWithSsrFGuard** | **src/infra/net/fetch-guard.ts** | **24** | **10** | **240** |
| **resolveCommandSecretRefsViaGateway** | **src/cli/command-secret-gateway.ts** | **15** | **14** | **210** |
| **fetchRemoteMedia** | **src/media/fetch.ts** | **18** | **11** | **198** |
| **loadModelCatalog** | **src/agents/model-catalog.ts** | **18** | **11** | **198** |
| **resolveApiKeyForProvider** | **src/agents/model-auth.ts** | **13** | **15** | **195** |
| **start** | **src/gateway/client.ts** | **12** | **13** | **156** |
| **resolveAuthProfileOrder** | **src/agents/auth-profiles/order.ts** | **11** | **11** | **121** |
| **createClackPrompter** | **src/wizard/clack-prompter.ts** | **10** | **11** | **110** |

### 发现僵尸函数

为了进一步分析整体的分布，我们尝试所有函数的fan-in和fan-out绘制成直方图。鉴于嵌入式图数据库NeuG提供了完善的Python SDK，我们可以方便拿到cypher查询结果，并进一步利用Python各类数据分析工具进一步统计。可以发现大部分的函数fan-in/fan-out值均小于2，说明openclaw大部分代码都是线性执行的，即这些函数在调用链路上处于“一进一出”的状态。
![Figure_1.png](https://intranetproxy.alipay.com/skylark/lark/0/2026/png/123756468/1774104504244-cf00151a-5aff-437f-b012-55646a6045a8.png?x-oss-process=image%2Fformat%2Cwebp)

此外，我们还发现有超过20%的函数的fan-in竟然为零，也就是说这些函数没有被任何其他函数调用。即使经过统计后排除一部分入口函数，还剩余两千多个真正的僵尸函数，以assertPublicHostname@/src/infra/net/ssrf.ts这个函数为例，在最新的仓库中完全没有被调用，进一步检查git修改记录后发现，这个函数在5bd550这个commit中被首次引入，并且当时是被实际调用的，而在b62355这个commit中，其功能被迁移至resolvePinnedHostname这个函数。可能是出于稳定性考虑，后续的开发者也不敢随意删除这些代码，因此就以僵尸函数的形式遗留了下来。

这些都是快速迭代的副产品。僵尸代码在频繁的代码改动中缺少维护，最终遗留在代码库中。这些代码不仅仅会让整个代码库变得臃肿，它的真正危害在于：新来的贡献者看到这些函数，不知道它们已经没人用了，可能会基于死代码写新功能，或者 debug 的时候钻进一条死胡同。在 OpenClaw 这种日更项目里，20% 的僵尸代码率是一笔不小的认知税。

---

## 发现二：高耦合模块

我们分析了最近 20 个 bug，57 个根因候选函数中，**24 个落在 `src/agents` 模块**——占比 42%。我们想探索一下，为什么这么多高比例的bugs都在`agents` 模块中。

我们通过Cypher查询，查看 `agents` 与其他模块的调用关系：

```cypher
MATCH (m1:Module {name: 'agents'})<-[:BELONGS_TO]-(f1:File)-[:DEFINES_FUNC]->(func1:Function)
MATCH (func1)-[:CALLS]->(func2:Function)<-[:DEFINES_FUNC]-(f2:File)-[:BELONGS_TO]->(m2:Module)
WHERE m2.name <> 'agents'
RETURN m2.name, count(*) as call_count
ORDER BY call_count DESC
LIMIT 10
```

查询结果如下，下面的数字表示两个模块之间的​**函数调用次数**​（a→b 表示 a 模块的函数调用 b 模块的函数多少次）：

```
agents <-> reply    a→b=19    b→a=117
agents <-> infra    a→b=108    b→a=15
agents <-> pi-embedded-runner    a→b=19    b→a=88
agents <-> tools    a→b=38    b→a=55
agents <-> plugins    a→b=45    b→a=40
agents <-> gateway    a→b=9    b→a=60
agents <-> src    a→b=35    b→a=31
agents <-> models    a→b=1    b→a=63
agents <-> sessions    a→b=46    b→a=5
agents <-> auth-profiles    a→b=24    b→a=12
```

我们发现，实际上有30多个模块与 `agents` 有双向的函数依赖。例如， `agents`调用了19次`reply`，而 `reply`更是调用 117 次`agents` ——大半个系统的业务逻辑最终都要经过这里。我们也将他们之间的函数调用通过图的可视化（neug-ui）展示如下：
![](https://intranetproxy.alipay.com/skylark/lark/0/2026/png/123756468/1774003467389-815c2060-7900-4da3-bfdd-b1fee73d2adc.png)

这意味着`agents` 是系统的结构性枢纽，改一行代码的涟漪可传到半个项目。Bug 扎堆不是巧合，是结构使然。

---

---

## 结构性问题的价值

以上发现共同指向一个事实：

> 当代码库规模超过人脑的处理极限，结构性问题只能靠**结构查询**发现。

传统工具能告诉你：

- 哪行代码有语法错误
- 哪个函数复杂度太高

NeuG 能告诉你：

- 一次代码修改会影响多少函数
- 哪些代码其实已经没人用了
- 哪个模块是系统的枢纽

这些答案藏在**关系**里，不在字符串里。

---

## 给开发者的建议

基于这次分析，三件性价比最高的事：

1. **解耦高耦合模块**：`src/agents` 与多个模块双向缠绕，定义清晰的接口边界能降低风险。
2. **关注高风险函数**：`createConfigIO`、`loadSessionStore` 等函数动一发牵全身，优先补充测试覆盖。
3. **清理死代码**：清除不需要的僵尸代码，给后来者省大量困惑。


---

## 结语

OpenClaw 的 18,000 次提交代表了快速迭代的活力，但也积累了结构性债务。NeuG 的价值在于：让这种"看不见的债务"变得**可视、可查询、可分析**。

当你的代码库也有几千个文件、几万个函数时，grep 和 review 可能不够用了。这时候，你需要的是一张图。

---

*本文分析基于 NeuG 图数据库和 CodeScope 代码分析引擎。完整数据索引耗时约 4 分钟。*

