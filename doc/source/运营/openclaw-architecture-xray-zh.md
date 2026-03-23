# 解剖 OpenClaw：代码知识图谱下的高速迭代 AI 网关

> *索引 6,000+ TypeScript 文件和 18,000 次提交之后，我们想搞清楚两件事：某些 bug 为什么反复出现？以及，快速迭代到底在暗处欠下了什么债。*

---

## 缘起

[OpenClaw](https://github.com/openclaw/openclaw) 是一个多渠道 AI 网关，负责在 Telegram、Discord、Slack、MS Teams、飞书、Matrix 等等消息平台和各家 LLM 之间搬运消息。18,000+ 次提交，版本号天天跳——这个项目跑得很快。跑得快的副作用是，结构性的债务会悄悄堆在犄角旮旯，`grep` 翻不出来，review 也未必看得见。

我们用 [CodeScope](https://github.com/codescope/codescope)（一个基于图数据库 + 向量索引的代码分析引擎）给 OpenClaw 做了一次全库扫描，产出了一张代码知识图谱：**21,057 个函数**、**35,761 条调用边**、**25,883 条导入关系**、**318 个模块**，全部可以用 Cypher 查询。

下面的内容不是代码审查，更像是给代做了一次深度透视。

---

## 数字说话


| 指标       | 数值        |
| ---------- | ----------- |
| 源文件     | 6,062       |
| 函数       | 21,057      |
| 调用边     | 35,761      |
| 导入边     | 25,883      |
| 类         | 233         |
| 模块       | 318         |
| 索引的提交 | 500（最近） |
| 索引时间   | ~4 分钟     |

两万个函数被三万六千条调用边连在一起——光看这个密度就知道，里面的依赖关系不会简单。

---

## 发现 1：Bug 往哪扎堆？答案指向同一个地方

我们拿最近 20 个标记为 bug 的 issue 做了一次批量根因分析——让 CodeScope 对每个 bug 提取文件路径、函数名、stack trace，和图谱做交叉匹配，找出最可能的根因函数。然后把结果按模块聚合，看看哪个模块反复上榜：


| 模块              | Bug 出现次数 |
| ----------------- | :----------: |
| **src/agents**    |    **24**    |
| src/infra         |      4      |
| src/telegram      |      4      |
| extensions/feishu |      4      |
| src/auto-reply    |      3      |
| src/cli           |      3      |
| extensions/matrix |      3      |

20 个 bug，每个 bug 产出若干根因候选，一共 57 个嫌疑函数——其中 24 个落在 `src/agents` 里。这个模块不是在参与 bug，它是 bug 的常驻地址。

为什么？看看耦合数据。下面的数字表示两个模块之间的**函数调用次数**（a→b 表示 a 模块的函数调用 b 模块的函数多少次）：

```
agents <-> commands    a→b=  1   b→a=140
agents <-> infra       a→b=116   b→a= 16
agents <-> reply       a→b= 13   b→a=113
agents <-> tools       a→b= 35   b→a= 63
agents <-> export-html a→b= 95   b→a=  0
agents <-> pi-embedded a→b= 14   b→a= 73
```

六个模块跟 `agents` 缠在一起。其中 `commands` 调用 `agents` 140 次，`reply` 调用 113 次，`pi-embedded` 调用 73 次——大半个系统的业务逻辑最终都要经过这里。反过来 `agents` 又往外伸了不少手：调 `infra` 116 次，调 `export-html` 95 次。

这意味着你在 `agents` 里改一行代码，涟漪可以传到半个项目。bug 扎堆在这里不是巧合，而是结构使然。

---

## 发现 2：心跳问题——一个"小 Bug"背后的架构困局

Issue [#43767](https://github.com/openclaw/openclaw/issues/43767) 的描述很直白：

> *"心跳忽略 `lightContext: true`，加载完整 agent 上下文 + 无界会话历史"*

报告者说的是：HEARTBEAT.md 里明明写了 `lightContext: true`，要求心跳只加载轻量上下文。但实际上每次心跳都把整个 workspace 的文件塞进去（AGENTS.md、SOUL.md、TOOLS.md……），系统 prompt 膨胀到 29K 字符，会话历史无限增长，最终撞上模型的 200K token 上限。

乍一看，这就是个配置标志没被读到。加个 `if (lightContext)` 不就完了？

但用 CodeScope 展开 `runHeartbeatOnce` 的调用树之后，事情就不那么简单了：

```
runHeartbeatOnce
  ├── parseAgentSessionKey        → src/sessions/
  ├── resolveDefaultAgentId       → src/agents/
  ├── resolveHeartbeatConfig      → src/infra/heartbeat-runner.ts
  ├── isWithinActiveHours         → src/infra/heartbeat-active-hours.ts
  ├── resolveHeartbeatPreflight   → src/infra/heartbeat-runner.ts
  │   ├── resolveHeartbeatSession     → src/infra/heartbeat-runner.ts
  │   ├── resolveAgentWorkspaceDir    → src/agents/
  │   ├── resolveHeartbeatReasonFlags → src/infra/heartbeat-runner.ts
  │   └── peekSystemEventEntries      → src/infra/system-events.ts
  ├── resolveHeartbeatDeliveryTarget → src/infra/outbound/targets.ts
  │   ├── normalizeChatType           → src/channels/
  │   ├── resolveOutboundChannelPlugin → src/infra/outbound/
  │   └── resolveSessionDeliveryTarget → src/infra/outbound/
  ├── resolveEffectiveMessagesConfig → src/agents/identity.ts
  ├── resolveHeartbeatRunPrompt     → src/infra/heartbeat-runner.ts
  ├── buildOutboundSessionContext   → src/infra/outbound/session-context.ts
  ├── getChannelPlugin              → src/channels/plugins/
  └── deliverOutboundPayloads       → src/infra/outbound/deliver.ts
```

一个"轻量定时器"，调用链穿过 **7 个子系统**：sessions、agents、infra、channels、outbound delivery、system events、plugins。`heartbeat-runner.ts` 这一个文件就导入了 **34 个模块**。

问题就在这里：这条心跳路径在一轮轮迭代中被"顺手"挂上了越来越多的依赖——解析 agent 配置、加载 workspace、构建消息上下文、选择投递渠道、获取 channel 插件……到最后它和完整的消息处理管线走的几乎是同一条路。这时候你加一个 `lightContext` 标志，效果约等于给一辆满载的卡车贴一张"轻型车辆"的标签：标签在，但它该重还是重。

这其实是快速演进项目的一个经典模式。功能刚写的时候确实简单，但每次有人"顺便"在里面多接一根管，不知不觉就和当初想要回避的重型路径长成了一个样子。这类问题靠 review 很难发现，因为每一次单独的改动都合理，只有把调用链整个铺开看才能意识到事情失控了。

---

## 发现 3：扩展质量的照妖镜——飞书 vs MS Teams

OpenClaw 有大量的渠道扩展（extensions），质量参差不齐。我们发现了一种有趣的方法来量化这种差异：看 CodeScope 分析 bug 时能不能精确定位到函数。

先解释一下 CodeScope 的根因评分机制。它综合了多种信号：

- **直接提及 (+1.0)**：bug 报告的 body 或 stack trace 里直接出现了函数名
- **文件路径匹配 (+0.8)**：函数所在文件被 bug 报告提到了（说明大方向对，但不确定具体是哪个函数）
- **语义匹配 (+0~1.0)**：函数的签名/文档和 bug 描述语义相关
- **调用链关联 (+0.5/跳)**：函数调用了被提及的函数，或被提及的函数调用了它

分数越高，嫌疑越大。关键在于，**不同类型的信号给出的精度完全不同**。

### 飞书：所有函数一样可疑

我们分析的 20 个 bug 中有 3 个击中了同一个文件：`extensions/feishu/src/streaming-card.ts`。拿 Issue [#43704](https://github.com/openclaw/openclaw/issues/43704) 为例：

> *"飞书流式卡片在 agent 产生多个输出块时合并了不相关的回复"*

用户在 issue 里提到了出问题的文件路径，但没有指出具体函数。CodeScope 的分析结果：


| 函数                      | 分数 | 分数来源     |
| ------------------------- | :--: | ------------ |
| `resolveApiBase`          | 0.80 | 文件路径匹配 |
| `resolveAllowedHostnames` | 0.80 | 文件路径匹配 |
| `getToken`                | 0.80 | 文件路径匹配 |

三个函数得分一模一样，都是 0.80——全部来自"这个函数在被提到的文件里"这一条信号。换句话说：我们知道问题出在这个文件，但**完全分不清具体是哪个函数的锅**。bug 报告没提到函数名，语义搜索也没能把它们区分开。

这说明 `streaming-card.ts` 内部的职责划分有问题。文件里的函数叫 `resolveApiBase`、`resolveAllowedHostnames`、`getToken`——光看名字都跟"流式卡片合并"没什么关系，实际的流式处理逻辑大概率混在某个大函数里，没有被拆分成有明确语义的小函数。

### MS Teams：直接锁定目标

对比 MS Teams 的 bug [#43648](https://github.com/openclaw/openclaw/issues/43648)：

> *"MS Teams 内联粘贴的图片下载失败（hostedContents API 返回空）"*

用户不仅提到了文件路径 `extensions/msteams/src/attachments/graph.ts`，还在 issue 里直接写了两个函数名 `downloadGraphHostedContent` 和 `buildMSTeamsGraphMessageUrls`。CodeScope 的结果：


| 函数                         | 分数 | 分数来源                               |
| ---------------------------- | :--: | -------------------------------------- |
| `downloadGraphHostedContent` | 1.50 | 直接提及 (1.0) + 文件路径匹配 (0.5)    |
| `downloadMSTeamsGraphMedia`  | 1.30 | 直接提及 (1.0) + 文件路径匹配 (0.3)    |
| `resolveMSTeamsInboundMedia` | 1.24 | 文件路径匹配 (0.8) + 调用链关联 (0.44) |

注意第三个函数 `resolveMSTeamsInboundMedia`——它没被用户提到，但 CodeScope 通过调用图发现它调用了 `downloadGraphHostedContent`，所以加上了调用链关联分。排名靠前的函数各有各的理由，分数拉开了梯度。

这说明 MS Teams 扩展的代码拆分做得不错：函数名和职责一一对应，`downloadGraphHostedContent` 干的就是下载 hosted content，一听名字就知道和"图片下载失败"相关。代码结构好 → 函数名有意义 → 用户报 bug 时自然能指出函数 → 工具也能精确定位。

### 这说明什么

同样是扩展，同样是 bug，一个只能定位到文件级（"在这个文件里，但不知道哪个函数"），另一个能精确定位到函数级甚至调用链。差异不在工具，在代码本身。

结构好的代码，连 bug 都好修。

---

## 发现 4：结构热点——风险最高的八个函数

CodeScope 有一个"热点"指标：`fan_in × fan_out`。fan-in 是多少个函数调用你，fan-out 是你调用多少个函数，乘积可以理解为"爆炸半径"——如果这个函数出了 bug，影响面有多大。


| 函数                       | Fan-in | Fan-out | 风险值 | 文件                                        |
| -------------------------- | :----: | :-----: | :----: | ------------------------------------------- |
| `createConfigIO`           |   18   |   59   | 1,140 | src/config/io.ts                            |
| `startGatewayServer`       |   10   |   87   |  968  | src/gateway/server.impl.ts                  |
| `runEmbeddedPiAgent`       |   14   |   61   |  930  | src/agents/pi-embedded-runner/run.ts        |
| `resolveAgentRoute`        |   32   |   20   |  693  | src/routing/resolve-route.ts                |
| `runCronIsolatedAgentTurn` |   10   |   56   |  627  | src/cron/isolated-agent/run.ts              |
| `loadSessionStore`         |   64   |    8    |  585  | src/config/sessions/store.ts                |
| `getReplyFromConfig`       |   20   |   24   |  525  | src/auto-reply/reply/get-reply.ts           |
| `runMessageAction`         |   19   |   21   |  440  | src/infra/outbound/message-action-runner.ts |

`createConfigIO` 排第一，风险值 1140。18 个地方在调用它，它自己又调用 59 个函数——动它一下，整个配置加载链条都可能出问题。有意思的是，配置解析这种东西往往不会被当作核心逻辑来写测试，但按结构风险算，它才是全库最要紧的函数。

`loadSessionStore` 也值得单独说。它的 fan-in 高达 64——Mattermost 的模型选择器在用它，语音通话模块在用它，ACP 会话管理在用它，subagent 的控制逻辑也在用它。查一下图谱，单跳就返回 20 多个调用者，分布在 8 个不同的模块。但这个函数的路径是 `src/config/sessions/store.ts`——名字听上去人畜无害，其实半个系统悄悄挂在它身上。

---

## 发现 5：2,081 个死函数——速度的代价

CodeScope 扫出了 **2,081 个零调用者的函数**——差不多是代码库的 10%。按模块分布：


| 模块       | 死函数数 |
| ---------- | :------: |
| diffs      |   158   |
| agents     |   145   |
| gateway    |   137   |
| infra      |   117   |
| channels   |   116   |
| auto-reply |    72    |
| discord    |    71    |
| cli        |    67    |
| a2ui       |    66    |
| voice-call |    61    |
| browser    |    59    |

这些都是快速迭代的副产品。`diffs` 扩展独占 158 个死函数，大概率是整块功能重写之后旧代码没删。`agents`（145）和 `gateway`（137）也是同样的故事——重构了好几轮，老路径还留着。

死代码不只是碍眼。它的真正危害在于：新来的贡献者看到这些函数，不知道它们已经没人用了，可能会基于死代码写新功能，或者 debug 的时候钻进一条死胡同。在 OpenClaw 这种日更项目里，10% 的死代码率是一笔不小的认知税。

---

## 发现 6：谁在被反复修改？

热/冷分析看的是：在最近 200 个回填提交里，每个模块有多大比例的函数被改动过。密度高的模块就是开发者正在集中投入精力的地方。


| 模块                                     | 函数总数 | 被修改的 | 密度 |
| ---------------------------------------- | :------: | :------: | :--: |
| extensions/telegram/src                  |    5    |    1    | 0.20 |
| src/agents/pi-extensions/context-pruning |    21    |    4    | 0.19 |
| src/cli/nodes-cli                        |    38    |    6    | 0.16 |
| src/terminal                             |    40    |    6    | 0.15 |
| src/infra/net                            |    47    |    6    | 0.13 |
| src/agents/pi-embedded-runner            |   211   |    23    | 0.11 |
| src/tts                                  |    58    |    6    | 0.10 |

`pi-embedded-runner` 有 211 个函数，200 次提交改了其中 23 个——它是体量最大的活跃变更模块。再想想它和 `agents` 之间 73 条调用线——高变更叠加高耦合，这种组合在架构层面最容易出问题。

`context-pruning`（密度 0.19）的情况更值得关注。21 个函数里有 4 个在近期被反复修改，将近五分之一。联系到前面 issue #43767 里"上下文无限膨胀"的问题——这个上下文裁剪模块被改得这么勤，某种程度上佐证了团队确实在跟上下文管理这块硬骨头较劲，而且还没找到稳定的方案。

---

## 图谱能看到什么

代码审查抓的是函数级别的问题，静态分析管的是语法和 import 级别的事情。图谱分析填补的是这两者之间的空白——**跨文件、跨模块的结构性问题**：

1. **谁依赖谁，方向和力度** — 不只是"A import 了 B"，而是"A 调 B 116 次，B 调 A 16 次"。谁是服务提供方谁是消费方，力度差多少，一查便知。
2. **一次修改的影响范围** — `loadSessionStore` 有 64 个调用者分布在 8 个模块。改它的函数签名等于同时动 8 个模块。`grep` 能找到调用点，但不能告诉你这些调用点分布在多少个独立模块里、影响有多大。
3. **依赖是怎么慢慢堆上去的** — 心跳运行器一开始可能就几行代码，现在导入了 34 个模块。图谱把这种渐进的膨胀量化出来。
4. **Bug 在结构层面的聚集** — 57 个嫌疑函数里 24 个指向 `src/agents/`，这种模式你一个一个读 issue 是很难察觉的。得跨 issue 聚合之后才看得到。
5. **哪些代码其实已经没人用了** — 一个扩展里趴着 158 个死函数，说明这里经历过大规模重写但没有清理。单看文件级别的 diff 很难发现这一点，需要全库级别的调用链分析。

这些信息不是用来修具体 bug 的，而是用来做架构层面的决策：哪里该优先解耦、哪个模块的接口该重新设计、清理哪块死代码性价比最高。

---

## 我们是怎么做的

整个分析基于 CodeScope，底层用 NeuG 做图存储，Zvec 做向量索引：

```bash
# 索引代码库（6K 文件约 4 分钟）
codegraph init --repo /tmp/openclaw --lang auto --commits 500 --backfill-limit 200

# 生成架构报告
codegraph analyze --db /tmp/openclaw_db --output openclaw-report.md

# 分析前 20 个 bug
codegraph analyze-bugs openclaw openclaw --db /tmp/openclaw_db --top 20 --label bug
```

深入的图谱查询用 Python API + Cypher：

```python
from codegraph.core import CodeScope
cs = CodeScope('/tmp/openclaw_db')

# runHeartbeatOnce 的完整调用树（2 跳）
rows = list(cs.conn.execute("""
    MATCH (f:Function {name: 'runHeartbeatOnce'})-[:CALLS]->(t1:Function)
    OPTIONAL MATCH (t1)-[:CALLS]->(t2:Function)
    RETURN f.name, t1.name, t1.file_path, t2.name, t2.file_path
"""))

# 谁依赖 loadSessionStore？
callers = list(cs.conn.execute("""
    MATCH (c:Function)-[:CALLS]->(f:Function {name: 'loadSessionStore'})
    RETURN c.name, c.file_path
"""))

# Bug 根因分析
result = cs.analyze_issue('openclaw', 'openclaw', 43767, topk=10)
```

以前要花几天人工排查的事情，一条 Cypher 就能问出来："谁传递依赖了 session store？""哪些模块和 agents 是双向耦合的？"这是结构查询，不是文本搜索——因为你要的答案藏在关系里，不在字符串里。

---

## 给 OpenClaw 的三个建议

基于这次分析，如果让我们挑三件性价比最高的事情来做：

1. **把 `src/agents` 拆开。** 六个模块跟它双向缠在一起，改一处动全身。不一定要大重构，先拆成 `agents/core`、`agents/tools`、`agents/subagent` 三块，定义好各自的边界和接口，耦合度就能降一截。
2. **给心跳造一条独立的轻量路径。** 现在的心跳和完整消息处理走的是同一条管线，靠 `lightContext` 标志来"假装轻量"。与其在重型管线上打补丁，不如从头写一个真正只做心跳该做的事的运行器——不加载 workspace，不构建完整上下文，不经过 34 个 import。
3. **清理扩展里的死代码。** `diffs` 的 158 个死函数、`discord` 的 71 个，留着没有任何好处。对着死代码报告过一遍，半天时间就能给后来的贡献者省掉大量困惑。

---

*本文所有数据由 [CodeScope](https://github.com/codescope/codescope) 使用 NeuG 嵌入式图数据库生成。完整索引（21K 函数、36K 调用边、500 次提交）在一台 MacBook 上不到 5 分钟跑完。*
