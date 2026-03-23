# Dissecting OpenClaw: What a Code Knowledge Graph Reveals About a Fast-Moving AI Gateway

> *How we indexed 6,000+ TypeScript files and 18,000 commits to understand why certain bugs keep appearing — and what it tells us about the hidden architectural cost of rapid iteration.*

---

Most architectural problems don't show up in a single pull request. They accumulate — one reasonable dependency at a time — until a "lightweight heartbeat timer" is importing 34 modules and a single config flag can't save you.

[OpenClaw](https://github.com/openclaw/openclaw) is a multi-channel AI gateway — a system that routes messages between dozens of messaging platforms (Telegram, Discord, Slack, MS Teams, Feishu, Matrix...) and LLM providers. It's fast-moving: 18,000+ commits, hundreds of contributors, and a version that bumps daily. When a project evolves this fast, structural debt accumulates in places you can't see with `grep` or a code review.

We used [CodeScope](https://github.com/codescope/codescope) — a graph + vector code intelligence engine — to build a knowledge graph of OpenClaw's entire codebase. The result: **21,057 functions**, **35,761 call edges**, **25,883 import relationships**, and **318 modules**, all queryable via Cypher.

This isn't a traditional code review. This is an architectural X-ray.

---

## By the Numbers

| Metric | Value |
|--------|-------|
| Source files | 6,062 |
| Functions | 21,057 |
| Call edges | 35,761 |
| Import edges | 25,883 |
| Classes | 233 |
| Modules | 318 |
| Commits indexed | 500 (most recent) |
| Indexing time | ~4 minutes |

The sheer scale is notable: over 21K functions connected by nearly 36K call edges. This is not a monolith — it's a dense web of interactions. And within that web, certain patterns jump out.

---

## Finding 1: The "God Module" — `src/agents`

Every project has a gravitational center. In OpenClaw, it's `src/agents`.

When we ran the bug root cause analysis against the 20 most recent bug reports, aggregating which modules appear most frequently as root cause candidates:

| Module | Bug appearances |
|--------|:-:|
| **src/agents** | **24** |
| src/infra | 4 |
| src/telegram | 4 |
| extensions/feishu | 4 |
| src/auto-reply | 3 |
| src/cli | 3 |
| extensions/matrix | 3 |

**`src/agents` is implicated in 24 out of 57 root cause candidates** across 20 bug reports. That's not a module — it's a nexus. And the coupling data explains why:

```
agents <-> commands    a→b=  1   b→a=140
agents <-> infra       a→b=116   b→a= 16
agents <-> reply       a→b= 13   b→a=113
agents <-> tools       a→b= 35   b→a= 63
agents <-> export-html a→b= 95   b→a=  0
agents <-> pi-embedded a→b= 14   b→a= 73
```

Six different modules have heavy bidirectional coupling with `agents`. The call flow patterns are revealing: `commands`, `reply`, `tools`, and `pi-embedded-runner` all *call into* `agents` heavily. Meanwhile, `agents` reaches *out* to `infra` (116 calls) and `export-html` (95 calls). This is a dependency hub where changes ripple outward in unpredictable ways.

---

## Finding 2: The Heartbeat Problem — When a Bug Report Reveals Architecture

Issue [#43767](https://github.com/openclaw/openclaw/issues/43767) reads:

> *"Heartbeat ignores `lightContext: true`, loads full agent context + unbounded session history"*

On the surface, this is a config flag not being respected. But when we trace it through the graph, something deeper emerges.

Using CodeScope's call graph, we extracted the full call tree of `runHeartbeatOnce` — the entry point for heartbeat ticks:

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

One function call reaches into **7 different subsystems**: sessions, agents, infra, channels, outbound delivery, system events, and plugins. And the file `heartbeat-runner.ts` alone imports **34 different modules**.

**This is the real insight**: the heartbeat bug isn't about a missing `if` check. It's about the fact that the heartbeat codepath — which was designed to be a lightweight periodic tick — has accumulated the same complex dependency chain as a full message processing pipeline. When you import 34 modules to send a periodic ping, the "light context" flag becomes architecturally meaningless. The function can't help but load heavy state, because its dependency surface *is* the heavy state.

This is a pattern we see repeatedly in fast-evolving projects: features that start simple accrue dependencies through incremental additions, until they're structurally indistinguishable from the heavyweight paths they were meant to avoid.

---

## Finding 3: The Extension Problem — Feishu Streaming as a Case Study

Three of the 20 bugs we analyzed hit the same file: `extensions/feishu/src/streaming-card.ts`. Issue [#43704](https://github.com/openclaw/openclaw/issues/43704):

> *"Feishu streaming card merges unrelated replies when agent produces multiple output blocks"*

When CodeScope traced this, the top candidates were all within the same file:

| Function | Score |
|----------|:-----:|
| `resolveApiBase` | 0.80 |
| `resolveAllowedHostnames` | 0.80 |
| `getToken` | 0.80 |

The score of 0.80 (file path match) for all three tells us something: the issue body explicitly mentions this file, and all functions inside it are equally suspect. This is a **high-cohesion, low-visibility** file — its internal structure is entangled, making it hard to isolate which function is responsible.

Contrast this with the MS Teams bug ([#43648](https://github.com/openclaw/openclaw/issues/43648)), where CodeScope pinpointed exactly:

| Function | Score | Explanation |
|----------|:-----:|-------------|
| `downloadGraphHostedContent` | 1.50 | Direct mention (1.0) + file match (0.5) |
| `downloadMSTeamsGraphMedia` | 1.30 | Direct mention + file match |
| `resolveMSTeamsInboundMedia` | 1.24 | File match + caller relationship |

The MS Teams extension has better-factored code — each function has a clear responsibility, so the bug maps precisely. The Feishu extension's streaming card handler is an undifferentiated blob.

**The takeaway**: extension code quality varies dramatically, and the graph reveals which extensions are well-factored (precise bug mapping) vs. entangled (all functions equally suspect).

---

## Finding 4: The Structural Hotspot Map

CodeScope's hotspot analysis ranks functions by structural risk: `fan_in × fan_out`. High fan-in means many callers depend on you. High fan-out means you depend on many callees. The product measures your "blast radius" — how much damage a bug in this function can cause.

| Function | Fan-in | Fan-out | Risk | File |
|----------|:------:|:-------:|:----:|------|
| `createConfigIO` | 18 | 59 | 1,140 | src/config/io.ts |
| `startGatewayServer` | 10 | 87 | 968 | src/gateway/server.impl.ts |
| `runEmbeddedPiAgent` | 14 | 61 | 930 | src/agents/pi-embedded-runner/run.ts |
| `resolveAgentRoute` | 32 | 20 | 693 | src/routing/resolve-route.ts |
| `runCronIsolatedAgentTurn` | 10 | 56 | 627 | src/cron/isolated-agent/run.ts |
| `loadSessionStore` | 64 | 8 | 585 | src/config/sessions/store.ts |
| `getReplyFromConfig` | 20 | 24 | 525 | src/auto-reply/reply/get-reply.ts |
| `runMessageAction` | 19 | 21 | 440 | src/infra/outbound/message-action-runner.ts |

`createConfigIO` (risk=1,140) is the most dangerous function in the codebase. With 18 callers and 59 callees, any change to it could break the config loading path for the entire system. Notice how it's in `src/config/io.ts` — config parsing is rarely tested as rigorously as business logic, yet it sits at the highest structural risk point.

`loadSessionStore` (fan-in=64) deserves special attention. It has 64 callers spread across the codebase — from Mattermost model pickers to voice call response generators to ACP session management. When we asked "who calls `loadSessionStore`?", the graph returned 20+ callers in a single hop, spanning 8 different modules. This is a function that every subsystem implicitly depends on, yet it lives in `src/config/sessions/store.ts` — a place that suggests "just configuration."

---

## Finding 5: 2,081 Dead Functions — The Cost of Speed

CodeScope found **2,081 functions with zero callers** — roughly 10% of the codebase. Distribution by module:

| Module | Dead functions |
|--------|:-:|
| diffs | 158 |
| agents | 145 |
| gateway | 137 |
| infra | 117 |
| channels | 116 |
| auto-reply | 72 |
| discord | 71 |
| cli | 67 |
| a2ui | 66 |
| voice-call | 61 |
| browser | 59 |

This is the sedimentary layer of rapid iteration. The `diffs` extension alone has 158 dead functions — likely an entire feature surface that was replaced but never cleaned up. The `agents` module (145 dead) and `gateway` (137 dead) both show the signature of frequent refactoring where old code paths are superseded but not removed.

Dead code isn't just clutter. It's a maintenance trap: contributors read dead code, mistake it for live code, and build on it. In a project with OpenClaw's velocity, this 10% dead code rate is a tax on every new contributor's understanding.

---

## Finding 6: The Modification Density Map

Which modules change the most? The hot/cold analysis (function modifications per total functions over 200 backfilled commits) reveals where the action is:

| Module | Functions | Modified | Density |
|--------|:-:|:-:|:---:|
| extensions/telegram/src | 5 | 1 | 0.20 |
| src/agents/pi-extensions/context-pruning | 21 | 4 | 0.19 |
| src/cli/nodes-cli | 38 | 6 | 0.16 |
| src/terminal | 40 | 6 | 0.15 |
| src/infra/net | 47 | 6 | 0.13 |
| src/agents/pi-embedded-runner | 211 | 23 | 0.11 |
| src/tts | 58 | 6 | 0.10 |

`pi-embedded-runner` (211 functions, 23 modified in 200 commits) is the largest actively-changing module. Combined with its coupling to `agents` (73 inbound calls), this is a high-churn, high-coupling module — the most architecturally fragile kind.

`context-pruning` (density 0.19) changes nearly one in five of its functions per commit batch. Given that issue #43767 (the heartbeat bug) involves unbounded context growth, finding that the context-pruning module is under heavy active modification suggests this is an area where the team is actively struggling with a hard problem.

---

## The Meta-Insight: What Graph Analysis Reveals That Single-File Tools Can't

Traditional code review can spot a bug in a function. Static analysis can find unused imports. But a code knowledge graph reveals **structural properties that no single-file tool can see**:

1. **Coupling patterns** — not just "A imports B" but "A calls B 116 times while B calls A 16 times." The asymmetry tells you who's the server and who's the client.

2. **Blast radius** — when `loadSessionStore` has 64 callers across 8 modules, changing its signature is an 8-module event. No `grep` can tell you the *structural importance* of that dependency.

3. **Dependency accumulation** — the heartbeat runner started as a simple timer but accumulated 34 imports. The graph shows this creep quantitatively.

4. **Bug hotspot convergence** — when 24 out of 57 root cause candidates point to `src/agents/`, that's not a coincidence you'd notice from reading individual issues. It requires aggregation across the graph.

5. **Dead code archaeology** — 158 dead functions in a single extension tells a story of replacement without cleanup that only emerges from whole-codebase analysis.

These are the kinds of insights that inform architectural decisions: where to invest in decoupling, which modules need better interfaces, where dead code cleanup will pay the highest dividend.

---

## How We Did It

The entire analysis was built using CodeScope with a NeuG graph database and Zvec vector index:

```bash
# Index the codebase (~4 minutes for 6K files)
codegraph init --repo /tmp/openclaw --lang auto --commits 500 --backfill-limit 200

# Generate the architecture report
codegraph analyze --db /tmp/openclaw_db --output openclaw-report.md

# Analyze top 20 bugs
codegraph analyze-bugs openclaw openclaw --db /tmp/openclaw_db --top 20 --label bug
```

For the deeper graph queries, we used the Python API with Cypher:

```python
from codegraph.core import CodeScope
cs = CodeScope('/tmp/openclaw_db')

# Full call tree of runHeartbeatOnce (2 hops)
rows = list(cs.conn.execute("""
    MATCH (f:Function {name: 'runHeartbeatOnce'})-[:CALLS]->(t1:Function)
    OPTIONAL MATCH (t1)-[:CALLS]->(t2:Function)
    RETURN f.name, t1.name, t1.file_path, t2.name, t2.file_path
"""))

# Who depends on loadSessionStore?
callers = list(cs.conn.execute("""
    MATCH (c:Function)-[:CALLS]->(f:Function {name: 'loadSessionStore'})
    RETURN c.name, c.file_path
"""))

# Bug root cause analysis
result = cs.analyze_issue('openclaw', 'openclaw', 43767, topk=10)
```

The graph makes it possible to ask questions that would otherwise require days of manual investigation: "Show me every function that transitively depends on the session store" or "Which modules are coupled to agents in both directions?" These are graph queries, not text searches. They require structure, not just string matching.

---

## Recommendations for OpenClaw

Based on this analysis, we'd suggest three high-leverage interventions:

1. **Factor `src/agents` into sub-modules with explicit interfaces.** The current coupling pattern (6+ modules with heavy bidirectional calls) makes `agents` a change amplifier. Even splitting it into `agents/core`, `agents/tools`, and `agents/subagent` with clean boundaries would reduce coupling.

2. **Create a lightweight heartbeat runtime.** The current heartbeat path imports 34 modules — the same dependency surface as full message processing. A dedicated lightweight runner that honors `lightContext` by construction (not by flag) would eliminate an entire class of bugs.

3. **Dead code sweep in extensions.** The 158 dead functions in `diffs` and 71 in `discord` are maintenance weight. A targeted cleanup pass, guided by the dead code report, would reduce cognitive load for contributors working in those areas.

---

## Try It Yourself

The full analysis pipeline is reproducible on any codebase:

```bash
# Index your own repo
codegraph init --repo /path/to/your/repo --lang auto --commits 500 --backfill-limit 200

# Run the architecture report
codegraph analyze --db /path/to/your/db --output report.md
```

- **CodeScope**: [github.com/codescope/codescope](https://github.com/codescope/codescope)
- **NeuG** (the embedded graph database): [github.com/TuGraph-family/tugraph-db](https://github.com/TuGraph-family/tugraph-db)

The full OpenClaw index — 21K functions, 36K call edges, 500 commits — was built in under 5 minutes on a MacBook. If you've ever wondered what your own codebase looks like as a graph, it's worth running.

---

*All data in this post was generated by [CodeScope](https://github.com/codescope/codescope) using the [NeuG](https://github.com/TuGraph-family/tugraph-db) embedded graph database.*
