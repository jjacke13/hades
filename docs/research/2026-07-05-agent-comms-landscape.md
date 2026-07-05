# Agent Communication and Cooperation: Landscape Survey

**Date:** 2026-07-05
**Context:** Comparing against hades's federated-blackboard-with-bridges model.
**Method:** LLM training knowledge (cutoff ~Aug 2025) — web tools were unavailable. Facts marked
`(verify)` may be post-cutoff or uncertain; spot-check the links before quoting.

---

## Introduction

The question "how do AI agents talk to each other?" splits immediately into two distinct concerns the
field often conflates: (1) **framework-level coordination** — how a specific library orchestrates
multiple LLM calls or role-playing agents, and (2) **protocol-level interoperability** — how
independently-deployed agents from different vendors exchange messages reliably. Hades sits in the
second camp: fully isolated processes that communicate only through explicit HTTP bridges, each
running its own Blackboard + Arbiter. This survey covers both concerns to establish where that design
sits in the landscape.

---

## 1. Multi-Agent Frameworks: Agent↔Agent Communication

### Microsoft AutoGen / AG2
**Paper:** "AutoGen: Enabling Next-Gen LLM Applications via Multi-Agent Conversation" (Wu et al., 2023, arXiv:2308.08155). **Repo:** `microsoft/autogen`.

AutoGen's primitives are **`ConversableAgent`** instances that communicate by passing chat messages
(OpenAI-format dicts) directly to each other via `initiate_chat()`. `GroupChat` adds a
`GroupChatManager` orchestrator that routes messages to the next speaker (round-robin, LLM-decides, or
custom callable). Communication model: **in-process shared Python runtime**. The `GroupChatManager`
holds the canonical message history and fans it out to all agents each turn — no isolation between
agents. AG2 (the 2024 fork, `ag2ai/ag2`, verify) adds a `Swarm` handoff mode but the core model is
unchanged.

**Pattern:** orchestrator-worker (GroupChat) + sequential P2P (two-agent). Shared in-process state.

### CrewAI
**Repo/docs:** `crewAIinc/crewai`.

Models a system as a **Crew** of **Agents** with **Roles** executing a list of **Tasks**. Two
cooperation mechanisms: (1) sequential task chaining — task N's output is string-injected into task
N+1's prompt; (2) a `delegate_work_to_coworker` tool that routes a natural-language request to another
named agent and returns the response synchronously. No persistent shared state or bus. **Pattern:**
orchestrator (Crew runner) + workers communicating via string injection and synchronous delegation.

### LangGraph
**Docs:** `langchain-ai/langgraph`.

Models workflows as a **directed graph of nodes** (callable, often an LLM call or tool) over a
**shared typed `State` dict**. Every node reads from and writes to the shared state; LangGraph merges
partial updates. Multi-agent: each node can be a compiled sub-graph (a full agent loop). Communication
= state mutations, not messages. LangGraph Platform/Cloud (verify, ~late 2024) adds deployment
persistence but does not change the communication model. **Pattern:** dataflow graph with shared
mutable state; closest to a "pipeline" architecture.

### OpenAI Swarm and Agents SDK
**Swarm:** `openai/swarm`, Oct 2024. Thin Python library demonstrating the **handoff** pattern: an
agent returns a `Result` whose `agent` field names another agent to receive control; the shared
conversation list is passed on. **Agents SDK** (`openai/openai-agents-python`, verify ~early 2025):
productizes Swarm — adds `handoff()`, `Runner.run()`, tracing, and "agent-as-tool" wrapping (an agent
can be called as a tool by another agent). Still in-process, shared conversation list. **Pattern:**
sequential state machine with handoffs; "agent-as-tool" is structurally the same as hades's `ask_agent`.

### MetaGPT and ChatDev
**MetaGPT paper:** arXiv:2308.00352 (Hong et al., 2023). **ChatDev paper:** arXiv:2307.07924 (Qian et al., 2023).

**MetaGPT** simulates organizational SOPs (PM, Architect, Engineer, QA). Agents publish **structured
artifacts** (PRDs, design docs, code) to a shared **`Environment`** — effectively a shared dict keyed
by message type. Other agents call `_observe()` to pull what their role cares about.
`Environment.publish_message()` is the coordination primitive. **This is the closest thing to a
blackboard in the mainstream LLM-agent framework space** — typed posts, role-filtered observation, no
direct agent-to-agent calls.

**ChatDev** uses strict sequential role-play (a "chat chain"): roles alternate conversationally. No
shared store beyond the transcript. More a study than a deployment architecture.

### CAMEL
**Paper:** arXiv:2303.17760 (Li et al., 2023). Two-agent role-play guided by an inception prompt.
Communication = back-and-forth chat. No shared state. More study than architecture.

### Magentic-One
**Paper:** arXiv:2411.04468 (Fourney et al., Microsoft Research, verify ~Nov 2024). An **Orchestrator**
agent maintains a **task ledger** and a **progress ledger** (structured markdown it updates each step).
Specialized workers (WebSurfer, FileSurfer, Coder, ComputerTerminal) are called by the Orchestrator as
tools, receive a prompt, return text. The ledgers are the Orchestrator's private state — workers are
stateless from its view. **Pattern:** strict orchestrator-worker, no peer communication.

### Other Notable Systems
- **BabyAGI / AutoGPT (2023):** single-agent task queues, not true multi-agent.
- **LlamaIndex Workflows:** typed `Event` objects emitted and subscribed in-process — structurally
  similar to a local event bus; closer to a blackboard than most, but still in-process.
- **Semantic Kernel Process Framework** (Microsoft, verify ~2024): plugin/kernel orchestration;
  multi-agent via Process Framework with shared kernel state.
- **Haystack Pipelines** (deepset): DAG-based, shared context dict, not multi-agent focused.

---

## 2. Inter-Agent Protocols and Standards

### Google Agent2Agent (A2A)
**Announcement:** Google I/O May 2025 (verify — post-cutoff). **Spec:** `google-a2a/a2a-spec` on GitHub.

A2A is a **protocol standard**, not a framework, designed for cross-vendor agent interoperability. Core concepts:

- **Agent Card:** a JSON document at `/.well-known/agent.json` describing the agent's capabilities,
  supported modalities, auth requirements, and endpoint URL. Discovery = HTTP GET.
- **Task lifecycle:** a Client Agent sends `tasks/send` (or `tasks/sendSubscribe` for streaming)
  JSON-RPC 2.0 to a Remote Agent. Task states: `submitted → working → input-required →
  completed/failed/canceled`. The remote agent streams partial results via SSE or returns them whole.
- **`input-required` state:** the remote agent signals it needs further input before continuing — maps
  directly to hades's `CONFIRM_REQUEST` / confirm-band.
- **Auth:** OAuth 2.0 / API keys / custom schemes declared in the Agent Card.
- **Transport:** HTTPS + JSON-RPC 2.0 + optional SSE.
- **Backers:** Google, Salesforce, SAP, Workday, Atlassian, Box, MongoDB, and others (verify exact
  list). Strong enterprise backing.
- **Relationship to MCP:** A2A explicitly positions itself as MCP's complement — MCP = agent↔tool;
  A2A = agent↔agent.

**A2A is the closest published standard to hades's Bridge `/ask` endpoint.** The semantic difference:
A2A models a `Task` with an explicit state machine and async polling/streaming; hades models a
`USER_MESSAGE` turn that returns a reply synchronously over one HTTP call.

### MCP (Model Context Protocol)
**Anthropic, verify ~Nov 2024.** Spec: `modelcontextprotocol/specification`.

MCP is **agent↔tool/context**, NOT agent↔agent. It defines how an LLM host connects to MCP Servers
that expose tools, resources, and prompts. Transport: stdio (subprocess) or HTTP+SSE. An MCP server
*could* expose another agent's functionality as a tool — the same pattern as hades's `ask_agent`
wrapping a peer's `/ask` endpoint. But MCP has no concept of peer discovery, task state machines, or
agent cards. Hades's subprocess-based native tools are MCP-adjacent in spirit (stdio, self-describing),
but hades pre-dates MCP in its own codebase and does not implement the protocol.

### ACP (Agent Communication Protocol)
**IBM / BeeAI, verify ~early 2025.** Spec: `i-am-bee/beeai-platform` / `agentcommunicationprotocol`.

ACP is IBM's answer in the same space as A2A. REST over HTTP, SSE for streaming, agent registration via
a registry/manifest. Defines `Run` objects with state (created / in-progress / awaiting / completed /
failed). Agents expose `POST /runs` and optional `GET /runs/{id}/events` SSE. ACP and A2A overlap
significantly; ACP was announced first and BeeAI had the more mature reference implementation; A2A has
more enterprise backing. Community bridging efforts exist (verify). Linux Foundation AI & Data is
involved.

### ANP (Agent Network Protocol)
**`AgentNetworkProtocol/agent-network-protocol` on GitHub, verify ~2024-2025.** Focused on
**decentralized agent identity** (W3C Decentralized Identifiers / DIDs) for discovery without a central
registry, plus end-to-end encryption between agents, and an agent description language (overlapping
with A2A's agent card). Less enterprise backing, more research-oriented. Targets open-network
interoperability; A2A targets enterprise OAuth-gated deployments. **(Relevant to Vaios's P2P /
hyperdht direction — DID identity is the open-network answer to peer discovery without a registry.)**

### AGNTCY
**`agntcy/agntcy` on GitHub, verify ~2025.** Cisco-backed (verify) initiative for agent
interoperability — an "Agent Connect Protocol" and an "Agent Directory" concept (agent marketplace /
discovery). Very early stage as of the cutoff.

### Historical Lineage: FIPA ACL / KQML / JADE

The modern protocols are rediscovering solutions the 1990s multi-agent research community worked out in detail:

- **KQML (1993):** first serious agent communication language. Defined **performatives** (speech act
  theory): `tell`, `ask-if`, `achieve`, `subscribe`, etc. A message's *intent* (performative) is
  separate from its *content*. Source: Finin et al., "KQML as an Agent Communication Language," CIKM 1994.
- **FIPA ACL (1997-2002):** standardized and extended KQML. Formal semantics, interaction protocols
  (Contract-Net, FIPA-Request, FIPA-Query), and two infrastructure services: **AMS** (Agent Management
  System — agent registry) + **DF** (Directory Facilitator — yellow-pages service discovery). Source:
  FIPA Specification Suite, fipa.org.
- **JADE (Java Agent DEvelopment Framework):** reference FIPA implementation. Agents registered with a
  platform; communicate via `ACLMessage` through a platform message queue. Platform provides AMS + DF +
  inter-platform transport (IIOP or HTTP). Source: Bellifemine et al., "JADE: A FIPA2000 Compliant
  Agent Development Environment," 2001.

**The wheel turns:** A2A's agent cards = FIPA DF entries. A2A's task lifecycle = a FIPA interaction
protocol. A2A's auth = FIPA's security extensions. The vocabulary is modern (JSON-RPC, OAuth) but the
concepts are 25 years old.

---

## 3. Architectural Patterns

### Orchestrator-Worker (Central Dispatcher)
The **dominant pattern in production today.** One master agent decomposes the task, dispatches subtasks
to specialized workers, collects results. Examples: Magentic-One (explicit), CrewAI (crew runner),
LangGraph (top-level graph), AutoGen GroupChatManager. OpenAI Agents SDK handoffs are a degenerate
case: the orchestration is a state machine where one agent is active at a time.

**Failure modes:** orchestrator is SPOF; it accumulates context (context-window pressure); workers are
stateless across calls.

### Peer-to-Peer / Flat Network
Rarely pure P2P in LLM-agent systems. CAMEL is the closest. AutoGen two-agent conversations are P2P.
True flat P2P multi-agent is mostly a research topic. Hades's bridge model is technically P2P — any
bridged agent can `/ask` any peer — but PeerLoopGuard + max_hops=1 effectively flatten it to 1-hop
delegation in v1.

### Hierarchical
Orchestrator-worker applied recursively. Supported by LangGraph subgraphs. Common in enterprise agent
platforms. Not qualitatively different from orchestrator-worker.

### Market / Auction / Contract-Net
Classic in FIPA/JADE (FIPA Contract-Net Interaction Protocol). Agents bid on tasks; a manager awards
work to the best contractor. Not used in LLM-agent systems as of the cutoff — LLMs are too slow and
expensive for tight auction loops. Active research area.

### Blackboard / Shared-Memory / Tuple Spaces
**Classic blackboard (HEARSAY-II, 1970s; Nii 1986):** a central shared data store where agents post
partial solutions; Knowledge Sources are triggered by patterns. No direct agent-to-agent communication.
**Linda tuple spaces (Gelernter, ACM TOPLAS 1985):** `out()`/`in()`/`rd()` typed tuples, content-based
access, temporal decoupling.

**MOOS (Newman, MIT 2003):** the direct ancestor of hades's Blackboard. Central MOOSDB, all variables
live there, apps post and subscribe, never call each other directly. Latest-value semantic (new post
overwrites previous) is a deliberate MOOS choice for a control loop: stale data should not queue up.

**Blackboard in LLM-agent systems: RARE.** MetaGPT's `Environment` (typed publish/observe) is the
closest mainstream analog. LlamaIndex Workflows use a typed event bus (structurally similar,
in-process). The rest of the field uses shared conversation lists, not blackboards.

**The federation of per-agent blackboards connected by bridges is architecturally novel in the
LLM-agent space.** Classical blackboard theory had ONE shared blackboard for the whole system. Hades
does the opposite: each agent keeps its blackboard private; data flows only through explicit bridge
channels. This is closer to **MOOS pShare** (selective variable mirroring between MOOSDB instances)
than to any classical blackboard system — and pShare is itself rarely seen outside the robotics
community.

### Actor Model
**Erlang (1986), Akka (Scala/Java):** isolated actors with private inboxes; communication only via
immutable messages to named actors; supervisors restart failed actors. Theoretically elegant for
multi-agent systems. Microsoft Orleans (virtual actors) has been proposed for LLM-agent orchestration
(verify, ~2024 papers). Uncommon in production LLM-agent frameworks.

**Hades vs actors:** Blackboard (pub/sub, latest-value, broadcast) vs actor (point-to-point mailbox,
queued). MOOS chose latest-value deliberately: a control loop cares about current state, not queued
history. This remains sensible for an LLM assistant.

---

## 4. Blackboard in the LLM-Agent World: How Common?

**Very uncommon at the system level; occasional inspiration at the framework level.**

The dominant coordination primitive across all major frameworks is a **shared conversation list** — a
growing array of OpenAI-format message dicts passed to each agent. This is not a blackboard: no typed
keys, no subscriber model, no event pump, no latest-value semantic.

| System | Blackboard-like feature | How close? |
|---|---|---|
| MetaGPT `Environment` | typed publish / role-filtered observe | Medium — typed, filtered; single global store, no push subscription |
| LlamaIndex Workflows | emit/subscribe typed Events | Close structurally; in-process only |
| MOOS / hades Blackboard | latest-value map, pub/sub, pump | The real thing (MOOS heritage) |
| Shared vector store (BabyAGI) | agents read/write embeddings to Pinecone | Weak — no subscription, no typing, no pump |
| LangGraph `State` | shared typed dict all nodes read/write | Shared state, not pub/sub; no event subscription |

**Finding:** Hades's per-agent Blackboard with no cross-agent sharing by default is **not replicated
anywhere in the mainstream LLM-agent landscape as of mid-2025.** The federation model (private
blackboard + bridge-only sharing) is genuinely unusual. The closest deployed analog in any domain is
MOOS itself.

---

## How Hades Compares

### Where hades is conventional

The `ask_agent` tool wrapping a peer's HTTP endpoint is structurally identical to OpenAI Agents SDK's
"agent-as-tool" pattern and to AutoGen's nested-chat pattern. Any framework that allows an agent to
call another agent as a function is doing the same thing. Hades's `/ask` endpoint looks, from the
outside, like a REST service any orchestrator framework can call — it could be made to speak A2A's
`tasks/send` with modest effort.

The shared-secret + peer-allowlist authentication is simple and correct for a private fleet. A2A and
ACP use OAuth 2.0/bearer tokens — more flexible, more complex. Hades's model is the right call for a
personally-operated setup and the right seam to upgrade later.

### Where hades is unusual

**The Blackboard itself.** No mainstream LLM-agent framework uses a local pub/sub blackboard as the
agent's internal coordination bus. Hades is the only known system porting MOOS's architecture to LLM
agents. The benefits: deterministic single-threaded dispatch, clean subscriber registration, a
well-understood concurrency model. The cost: unconventional, harder to explain to someone coming from
LangChain.

**Federation of private blackboards.** The entire framework world shares state by passing message
lists or mutating shared dicts. Hades's model — each agent fully isolated, sharing only what crosses
the bridge explicitly — is closer to Erlang's actor model or MOOS pShare than to any LLM-agent
framework. Benefits: no agent can accidentally corrupt another's context; bridge communication is
auditable, namespaced, auth-gated. Cost: no passive observation of a peer's state; all sharing is
explicit pull/push.

**Objectives as veto gates.** Most frameworks have no equivalent to hades's Objective (veto/confirm
gate on every tool call). CrewAI and AutoGen have "human in the loop" pause points, but they are not
structured as composable veto-stack modules that see every proposed action before it executes.
Magentic-One's Orchestrator makes analogous approval decisions but centrally, not as a pluggable stack.

**The architecture is a robotics port.** MOOS-IvP solved "distributed control loop with bounded trust
and explicit coordination" for AUVs. Hades imports that solution into the LLM-agent space. The
frameworks arrived at the same problems from the other direction (scale up a single LLM call to
multi-agent) and solved them ad hoc.

### Which standard hades most resembles

**Google A2A is the closest published standard.** Both model:
- An agent as an HTTP endpoint receiving a request and returning a response.
- Auth via a credential (A2A: OAuth/API key; hades: secret header).
- A confirmation/input-required state (A2A's `input-required` task state ≈ hades's `CONFIRM_REQUEST` /
  confirm-band auto-deny on peer turns).
- Agent discovery (A2A: agent card at `/.well-known/agent.json`; hades: static peer list in the manifest).

Hades's `/ask` is a simplified A2A `tasks/send` without the task state machine, async polling, or SSE streaming.

### Concrete borrow-able ideas

**1. A2A Agent Card for peer discovery.** Currently peers are statically declared in every manifest.
Serving a `/.well-known/agent.json` from BridgeModule (name, capabilities, endpoint URL) would let a
new agent discover existing peers dynamically — zero-config fleet registration. Cost: ~50 lines of JSON
generation + a GET handler in BridgeModule. No change to internal architecture.

**2. Async `/ask` with task-ID polling (A2A `tasks/send` pattern).** Today `/ask` blocks until the
peer's full turn completes (up to `ask_timeout_s`), holding the caller's turn open during the peer's
LLM call. A2A-style: `/ask` returns immediately with a `task_id`; the caller polls `/ask/{id}` or
subscribes `GET /ask/{id}/events` SSE. This is already the "ask offload" seam in hades's v2 list. A2A's
spec gives the exact API shape for free.

**3. Typed `/share` payloads (FIPA ontology pattern).** Currently `PEER.<from>.<key>` carries an opaque
string. Adding a `type` field (e.g., `{"type": "memory_fact", "content": "..."}`) would let the
MemoryModule or SkillsModule route incoming shared data by type without hardcoding key names — a small
schema change with large legibility gains. The concept is FIPA's `ontology`+`language` fields, brought
to one JSON field.

**4. Wildcard/prefix bus subscriptions for reactive peer-shared data.** Currently `/share` stores
`PEER.*` on the local Blackboard, but no module is automatically notified. If the Blackboard gained a
prefix-match subscription variant, a module could `subscribe("PEER.*", handler)` and react to
peer-pushed variables without polling. This is the MetaGPT `Environment._observe()` pattern. Cost: add
prefix matching to the Blackboard subscription table.

**5. ANP-style DID identity for open-network operation (v3 seam).** The current shared-secret model is
correct for a private fleet. For open-network interoperability with third-party agents (or a P2P /
hyperdht fleet without a central registry), W3C Decentralized Identifiers give each agent
cryptographically verifiable identity without a central registry. The `authorize()` seam in
BridgeModule is the right injection point. Cost: high (DID library + key management). File under v3;
do not build until the need is real.

---

## Sources and References

| Claim | Source |
|---|---|
| AutoGen conversable agents / GroupChat | arXiv:2308.08155 (Wu et al., 2023); github.com/microsoft/autogen |
| AG2 / Swarm handoff | github.com/ag2ai/ag2; openai/swarm |
| CrewAI roles/delegation | crewAIinc/crewai docs |
| LangGraph shared state | langchain-ai/langgraph docs |
| OpenAI Agents SDK handoffs | openai/openai-agents-python; platform.openai.com/docs/guides/agents (verify URL) |
| MetaGPT Environment/publish | arXiv:2308.00352 (Hong et al., 2023) |
| ChatDev chat-chain | arXiv:2307.07924 (Qian et al., 2023) |
| CAMEL role-play | arXiv:2303.17760 (Li et al., 2023) |
| Magentic-One orchestrator/ledger | arXiv:2411.04468 (Fourney et al., verify ~Nov 2024) |
| Google A2A protocol | google-a2a/a2a-spec; announced Google I/O May 2025 (verify) |
| MCP | modelcontextprotocol/specification; Anthropic verify ~Nov 2024 |
| ACP / BeeAI | i-am-bee/beeai-platform; IBM verify ~early 2025 |
| ANP | AgentNetworkProtocol/agent-network-protocol on GitHub (verify) |
| AGNTCY | agntcy/agntcy on GitHub; Cisco backing (verify) |
| KQML | Finin et al., "KQML as an Agent Communication Language," CIKM 1994 |
| FIPA ACL | FIPA Specification Suite, fipa.org |
| JADE | Bellifemine et al., "JADE: A FIPA2000 Compliant Agent Development Environment," 2001 |
| Blackboard / HEARSAY-II | Nii, "Blackboard Systems," AI Magazine, 1986 |
| Linda tuple spaces | Gelernter, "Generative Communication in Linda," ACM TOPLAS 1985 |
| MOOS / MOOSDB | Newman, "MOOS: Mission-Oriented Operating Suite," MIT 2003 |
| Actor model | Hewitt et al., 1973; Erlang/OTP; Akka docs |

---

*Generated from LLM training knowledge (cutoff ~Aug 2025), no live web access. `(verify)`-tagged facts
— especially A2A/ACP/ANP/AGNTCY dates and backers, which are 2025-era — should be link-checked before
citing externally.*
