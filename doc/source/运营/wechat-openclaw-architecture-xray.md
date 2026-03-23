# 我们给 OpenClaw 做了一次 X 光：bug 扎堆、依赖膨胀、10% 死代码全都藏在这里

[OpenClaw](https://github.com/openclaw/openclaw) 是最近很火的一个多渠道 AI 网关，在 Telegram、Discord、Slack、MS Teams、飞书、Matrix 等消息平台和各家 LLM 之间搬运消息。18,000+ 次提交，版本号天天跳。

跑得快的项目，结构性的债务会悄悄藏在犄角旮旯——`grep` 翻不出来，肉眼也未必看得见。

我们用代码知识图谱工具 [CodeScope](https://github.com/codescope/codescope) 给 OpenClaw 做了一次全库扫描：**21,057 个函数、35,761 条调用边、318 个模块**，全部连成图。下面是四个关键发现。

---

## 发现一：Bug 往哪扎堆？答案指向同一个地方

我们拿最近 20 个 bug issue 做了批量根因分析，把结果按模块聚合：

| 模块              | Bug 出现次数 |
| ----------------- | :----------: |
| **src/agents**    |    **24**    |
| src/infra         |      4       |
| src/telegram      |      4       |
| extensions/feishu |      4       |
| src/auto-reply    |      3       |

20 个 bug，57 个嫌疑函数，其中 **24 个落在 `src/agents`**。为什么？看耦合数据：

```
agents <-> commands    a→b=  1   b→a=140
agents <-> infra       a→b=116   b→a= 16
agents <-> reply       a→b= 13   b→a=113
agents <-> tools       a→b= 35   b→a= 63
agents <-> pi-embedded a→b= 14   b→a= 73
```

六个模块跟 `agents` 缠在一起，大半个系统的业务逻辑都要经过这里。你在 `agents` 里改一行代码，涟漪可以传到半个项目。**bug 扎堆在这里不是巧合，而是结构使然。**

---

## 发现二：一个"小 Bug"背后的架构困局

Issue #43767 描述很直白：

> *"心跳忽略 `lightContext: true`，加载了完整 agent 上下文 + 无界会话历史，系统 prompt 膨胀到 29K 字符，最终撞上模型的 200K token 上限。"*

乍一看，加个 `if (lightContext)` 不就完了？

但用图谱展开 `runHeartbeatOnce` 的调用链之后，事情就不那么简单了——这个"轻量定时器"实际上穿过了 **7 个子系统**，`heartbeat-runner.ts` 这一个文件就导入了 **34 个模块**。

这条心跳路径在一轮轮迭代中被"顺手"挂上越来越多的依赖，到最后和完整的消息处理管线走的几乎是同一条路。这时候加一个 `lightContext` 标志，效果约等于**给一辆满载的卡车贴一张"轻型车辆"的标签**：标签在，但它该重还是重。

每一次单独的改动都合理，只有把调用链整个铺开看才能意识到事情失控了——这正是图分析能看到、单文件工具看不到的地方。

---

## 发现三：风险最高的八个函数

CodeScope 用 `fan_in × fan_out` 衡量"爆炸半径"——如果这个函数出了 bug，影响面有多大：

| 函数                       | Fan-in | Fan-out | 风险值 |
| -------------------------- | :----: | :-----: | :----: |
| `createConfigIO`           |   18   |   59    | 1,140  |
| `startGatewayServer`       |   10   |   87    |  968   |
| `runEmbeddedPiAgent`       |   14   |   61    |  930   |
| `resolveAgentRoute`        |   32   |   20    |  693   |
| `runCronIsolatedAgentTurn` |   10   |   56    |  627   |
| `loadSessionStore`         |   64   |    8    |  585   |
| `getReplyFromConfig`       |   20   |   24    |  525   |
| `runMessageAction`         |   19   |   21    |  440   |

`createConfigIO` 风险值最高（1140）——18 个地方调用它，它自己又调用 59 个函数。有意思的是，配置解析往往不会被当作核心逻辑来写测试，但按结构风险算，它才是全库最要紧的函数。

`loadSessionStore` 的 fan-in 高达 64，从语音通话到 ACP 会话管理到 Mattermost 模型选择，散落在 8 个不同模块。它的路径是 `src/config/sessions/store.ts`——名字听上去人畜无害，其实半个系统悄悄挂在它身上。

---

## 发现四：2,081 个死函数——速度的代价

CodeScope 扫出了 **2,081 个零调用者的函数**，差不多是整个代码库的 **10%**：

| 模块       | 死函数数 |
| ---------- | :------: |
| diffs      |   158    |
| agents     |   145    |
| gateway    |   137    |
| infra      |   117    |
| channels   |   116    |
| discord    |    71    |

`diffs` 扩展独占 158 个死函数，大概率是整块功能重写之后旧代码没删。

死代码的真正危害不是占空间，而是**认知税**：新贡献者看到这些函数，不知道它们已经没人用了，可能会基于死代码写新功能，或者 debug 时钻进一条死胡同。在 OpenClaw 这种日更项目里，这笔税不小。

---

## 这说明什么

图谱分析和传统工具不是替代关系，而是视角的补充：

- `grep` 能找到调用点，但不能告诉你调用者分布在多少个独立模块
- 静态分析能抓语法问题，但看不见"这个模块被半个系统耦合着"
- 每一次 PR 改动都合理，只有把全局调用链铺开才能看见债务在哪里积累

**结构性的问题，需要结构性的工具才能看见。**

---

本文分析基于 [CodeScope](https://github.com/codescope/codescope)，底层使用 [NeuG](https://github.com/TuGraph-family/tugraph-db) 图数据库。完整索引（21K 函数、36K 调用边、500 次提交）在一台 MacBook 上不到 5 分钟跑完，感兴趣可以直接上手试试。

觉得有用，欢迎转给你的技术 leader 或团队。
