# 用 NeuG 给 OpenClaw 做代码体检，发现了这些结构性问题

![封面图建议：代码节点和调用关系构成的网络可视化图]

---

上一篇我们发现 OpenClaw 的心跳会悄悄吃掉大量 tokens。

这篇换个角度——**从开发者视角，看快速迭代留下了哪些结构性债务**。

---

## 把代码库变成一张图

代码的结构本质是图：

- **函数 = 节点**
- **调用关系 = 边**

我们用 NeuG 图数据库 + zvec 向量数据库，给 OpenClaw 建了一张代码知识图谱：

| 类型 | 数量 |
|------|------|
| 源文件 | 6,062 |
| 函数 | 21,057 |
| 调用边 | 35,761 |
| 导入边 | 25,883 |
| 模块 | 318 |

两万个函数，被三万六千条调用边连在一起。

有了这张图，之前很难回答的问题，变成了一条 Cypher 查询。

---

## 发现一：高危函数

我们定义了一个**风险值**：

> 风险值 = fan_in（被调用次数）× fan_out（调用别人次数）

代表这个函数出问题的"爆炸半径"。

一行代码找出全库热点：

```python
functions = cs.hotspots(topk=30)
```

**风险最高的 5 个函数：**

| 函数名 | fan-in | fan-out | 风险值 |
|--------|--------|---------|--------|
| startGatewayServer | 10 | 103 | **1030** |
| createConfigIO | 18 | 56 | **1008** |
| runEmbeddedPiAgent | 14 | 67 | **938** |
| loadOpenClawPlugins | 20 | 36 | **720** |
| runCronIsolatedAgentTurn | 11 | 60 | **660** |

以 `createConfigIO` 为例：被 18 处调用、自己调 56 个函数。

**配置解析往往没人写测试，但按结构风险算，它才是全库最危险的函数之一。**

进一步筛选"fan-in 和 fan-out 都 > 10"的函数，整个代码库只有 **17 个**满足条件——这才是真正需要重点关注的核心高危函数。

---

## 发现二：20% 的代码是"僵尸"

把全库函数的 fan-in 分布画成直方图：

![配图建议：fan-in 分布直方图，大量函数集中在 0-2 区间]

**超过 20% 的函数从来没被任何人调用。**

排除入口函数后，仍剩余 **2,000+ 个真正的僵尸函数**。

真实案例：`assertPublicHostname`（`src/infra/net/ssrf.ts`）

- commit `5bd550`：函数被引入，当时有人用
- commit `b62355`：功能迁移到 `resolvePinnedHostname`
- 结果：旧函数没被删，就这么"活死"在代码库里

**危害不只是碍眼：**

新开发者看到它 → 以为有用 → 基于它写新功能 → debug 时钻进死胡同

20% 僵尸率，是笔不小的认知税。

---

## 发现三：为什么 42% 的 Bug 来自同一个模块？

分析最近 20 个 bug，57 个根因候选函数里——

**24 个落在 `src/agents`，占比 42%。**

用 Cypher 查一下调用关系：

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
agents <-> pi-embedded-runner a→b=19    b→a=88
agents <-> models             a→b=1     b→a=63
agents <-> gateway            a→b=9     b→a=60
agents <-> tools              a→b=38    b→a=55
```

![配图建议：以 agents 为中心的模块调用关系可视化图（neug-ui 截图）]

**30+ 个模块与 `agents` 双向依赖。**

`reply` 调用 `agents` 117 次，`pi-embedded-runner` 调用 88 次——

大半个系统的业务逻辑都要经过这里。

**Bug 扎堆不是巧合，是结构使然。**

---

## NeuG 能看到什么

传统工具：语法错误、函数复杂度

NeuG 图数据库：

- 一次改动影响多少函数
- 哪些代码没人用了
- 哪个模块是系统枢纽

> 这些答案藏在关系里，不在代码行里。

---

## 给开发者的三条建议

**1. 解耦 `src/agents`**
30+ 个模块双向依赖，先定义清晰的接口边界。

**2. 给高风险函数加测试**
`createConfigIO`、`startGatewayServer` 动一发牵全身，优先补覆盖。

**3. 清理僵尸代码**
2,000+ 个死函数，对着调用图过一遍，给后来者省大量困惑。

---

整个分析基于 [NeuG](https://github.com/alibaba/neug) 图数据库开源项目，已打包为 [CodeGraph Skill](https://github.com/alibaba/neug/tree/main/skills/codegraph)，可直接复现。索引 6,000 文件约 4 分钟。

*本文分析基于 NeuG 图数据库和 CodeScope 代码分析引擎。*
