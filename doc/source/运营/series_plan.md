# 开源社区健康度系列文章规划

**数据基础**：`report_full.md`（完整报告）+ `report_short.md`（精简版）

**目标平台**：知乎、微信公众号、小红书（每篇覆盖三个平台版本）

---

## 拆分逻辑说明

两篇报告的视角互补：
- `report_full.md`：方法论驱动，按维度→排名→赛道→案例展开，数据完整
- `report_short.md`：结论驱动，先抛反直觉发现，再用对比案例支撑，传播性更强

拆分原则：
- 衰退类案例（LangChain/AutoGen/SD WebUI）合并为一篇，主题聚焦"协作网络无法沉淀"
- 成功案例（Elasticsearch/PyTorch/OpenClaw）合并为一篇，对比叙事：前两者是经过时间验证的治理范本，OpenClaw是新模式早期样本，结尾留悬念
- 赛道横向对比与技术选型建议合并，两者目标读者相同——技术选型者

---

## 最终5篇序列

| # | Topic | 主要素材来源 | 核心论点 |
|:---:|:---|:---|:---|
| 1 | **74个项目全排名 + 反直觉案例** | short为主，full补充完整排名 | Star数不等于健康度；Ollama/Next.js是典型反例，vLLM/DuckDB是意外黑马 |
| 2 | **成功范本对比：Elasticsearch、PyTorch 与 OpenClaw** | full案例 + short的OpenClaw vs AutoGPT对比 | Elasticsearch/PyTorch是经过时间验证的治理机制；OpenClaw是新模式的早期样本，结尾留悬念：能否成为下一个PyTorch？ |
| 3 | **三个AI明星项目的衰退解剖：LangChain、AutoGen、SD WebUI** | full为主 | 三条不同衰退路径（虚假开放/封闭衰亡/单点故障），共同指向协作网络无法沉淀 |
| 4 | **方法论：NeuG图数据库如何度量社区健康** | full方法论部分 | 时序协作图+四维评分的技术实现，NeuG能力的核心展示 |
| 5 | **技术选型者指南：十大赛道健康度对比 + 虚假繁荣识别** | full赛道分析 + short对比案例 | 生态位预设健康基准线；赛道/项目/信号三个层面的选型框架 |

---

## 三平台内容策略

| 平台 | 深度 | 风格 | 字数参考 |
|:---|:---:|:---|:---:|
| 知乎 | 完整数据+逻辑链+结论 | 分析性，结构清晰，中等专业深度 | 2000-4000字 |
| 微信公众号 | 核心结论+故事化叙述 | 结论先行，移动端友好，结尾有行动号召 | 1500-2500字 |
| 小红书 | 结论前置+数据高光 | 视觉为主，每张卡片一个结论 | 正文500字以内+图卡 |

---

## 各篇素材索引

**第1篇** 重点素材：
- short: 开头Ollama vs vLLM引子、五个反直觉发现、Top10/意外之选/Bottom5、三组对比案例（Ollama vs vLLM / OpenClaw vs AutoGPT / Next.js vs DuckDB）
- full: 完整74项目四维评分表

**第2篇** 重点素材：
- full: Elasticsearch案例（逆周期增长、核心不可达率从20%降至3.9%）、PyTorch案例（聚类系数稳定在0.41-0.54、核心不可达率从23%降至9.7%）、OpenClaw案例
- short: OpenClaw vs AutoGPT对比表、三条命运路径表格（AutoGPT着陆/LangChain低速/SD WebUI停摆）

**第3篇** 重点素材：
- full: LangChain vs AutoGen完整案例（平行爆发-衰退曲线、两种失败模式）、SD WebUI完整案例（时间线+Bus Factor=1）、关键发现2（虚假繁荣的数学特征：高事件量+低聚类系数）
- short: 三条衰退路径汇总表

**第4篇** 重点素材：
- full: 数据与图构建章节、三类时序图说明（G_AA/G_AR/G_AD）、四个评估维度设计、NeuG嵌入式模式技术描述

**第5篇** 重点素材：
- full: 赛道对比表（10个赛道平均分）、赛道生态位分析四点（底层沉淀效应/AI三层分化/应用层泡沫/中间件困境）、启示与建议
- short: Ollama vs vLLM对比（同赛道热度与健康度错位）、Next.js vs DuckDB对比（规模vs质量）、给技术选型者的建议

---

## 讨论备忘

- **第2篇的叙事方式**：采用方案B（对比叙事），Elasticsearch/PyTorch作为"经过时间验证的治理范本"，OpenClaw作为"新模式的早期样本"，结尾留悬念"OpenClaw能否成为下一个PyTorch？"——而非方案A（并列成功范本，弱化数据窗口短的限制）
- **第3篇的合并依据**：LangChain/AutoGen/SD WebUI三个项目衰退路径不同，但根因一致（协作网络无法沉淀），合并后论点更有力，分开写则各篇结论重复
- **第5篇的合并依据**：赛道横向对比（生态位决定健康基准线）和技术选型建议（如何识别虚假繁荣）的目标读者完全一致，且赛道数据天然是选型的数据依据，合并后逻辑更完整
- **report_short.md的独特内容**：Ollama vs vLLM、OpenClaw vs AutoGPT、Next.js vs DuckDB三组对比案例，以及"意外之选"板块，这些在report_full.md中没有，是传播性最强的素材，分配到第1、2、5篇
