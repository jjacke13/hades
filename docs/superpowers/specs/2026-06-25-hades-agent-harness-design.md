# hades — an AI agent harness on the MOOS-IvP architecture

**Status:** approved design (2026-06-25)
**Author:** Vaios K
**Scope:** C++ AI-agent harness that reuses the MOOS-IvP structural pattern — a central
publish/subscribe blackboard, pluggable modules, and a behavior-based multi-objective
decision "helm" — applied to a software, LLM-driven agent rather than a marine vehicle.

---

## 1. Motivation

MOOS-IvP is a battle-tested autonomy architecture for marine robots. Its distinctive
properties are worth borrowing for an AI agent:

- **Decoupling by a central blackboard.** Apps never call each other; they publish and
  subscribe to a central key/value store (the MOOSDB). A new capability is added by
  connecting a new app, with no change to existing ones.
- **Pluggable capability.** Apps (and behaviors) are loaded from config without
  recompiling the core. The user composes a system from parts.
- **Multi-objective arbitration.** The IvP helm does not pick one "winner" behavior
  (subsumption) nor average vectors (potential fields). Every behavior contributes a full
  utility surface over the action space; the helm fuses them by weighted sum and selects
  the single best *compromise* action. An autonomous agent, like an autonomous boat,
  cannot pause to ask a human to arbitrate every step — so a standing value function
  (weights) reconciles competing goals automatically.

This document maps those mechanisms onto an LLM agent and records the points where the
analogy breaks, so the implementation borrows the strengths without porting the parts
that do not transfer.

### 1.1 The user's framing (anchoring decisions)

- The **MOOSDB maps to the agent's central session** — one blackboard per session.
- **MOOS apps map to tools and capability modules** the user enables/adds (e.g. a tool,
  a chat-app interface). The harness is a **pluggable** system: users compose what they
  want.
- Purpose sits **between a research testbed and a reusable runtime**: instrument and
  experiment with behavior-arbitration, but with stable enough interfaces to build real
  agents on.

---

## 2. The MOOS → hades mapping

| MOOS-IvP | hades | Notes |
|---|---|---|
| MOOSDB (latest-value pub/sub) | **Blackboard** + **Eventlog** | MOOSDB keeps only the latest value per key; LLM agents are history-dependent, so the blackboard is paired with an append-only event log. |
| MOOS app (lifecycle, pub/sub) | **Module** | Event-driven, not fixed-Hz. |
| `pAntler` (launcher) | **Launcher** | Blackboard up first; reaps every child subprocess. |
| `.moos` mission file | **Manifest** (TOML) | One file declares modules, tools, objectives, weights, model. |
| `pHelmIvP` (helm) | **Arbiter** | Pluggable decision policy. |
| behavior (IvPBehavior) | **Objective** | Scores a discrete candidate-action set; may veto. |
| IvP function (utility surface) | **scored action-utilities** | Discrete, not continuous: plain scores, no ZAIC/branch-and-bound. |
| priority `pwt` | objective **weight** | Dominate, not veto — so a separate veto stage exists. |
| mode declarations | **phase** state-machine | Derived from predicates, never commanded directly. |
| run-states idle/running/active/complete | objective **lifecycle** | Completion is **re-armable** (MOOS's is permanent). |
| flags (endflag, runflag…) | objective **flags** | Blackboard side-effects that sequence objectives across turns. |
| AppCasting `buildReport` | module **status report** | Aggregated by a monitor; agent failures are silent/semantic. |
| `.alog` + `alogview`/`aloggrep` | **Eventlog** + replay/eval | grep-to-grade a run, like the headless MOOS harness. |
| scope/poke (`uXMS`/`uPokeDB`) | **scope** / **poke** | Live inspect + human-in-the-loop intervention. |
| PARK/DRIVE + `MOOS_MANUAL_OVERIDE` | **pause gate** | Paused agent keeps ingesting events, takes zero actions. |

### 2.1 Where the analogy breaks (load-bearing)

1. **Latest-value store is lossy.** Keep latest-value semantics for control/decision keys,
   but pair with an append-only Eventlog for transcript/tool history. *(Without this the
   agent has no memory.)*
2. **Fixed-Hz Iterate is the wrong default.** Modules block on LLM/tool calls for seconds.
   Primary path is **event-driven** (`on_event`); a slow `tick()` covers housekeeping
   (timeouts, budget decay, watchdog). The real budget is **tokens / cost / tool-calls**,
   not a Hz target.
3. **Discrete, heterogeneous, side-effecting actions** — not a continuous blendable
   domain. Drop branch-and-bound, piecewise-linear pieces, ZAIC, and OF_Reflector. Replace
   with **weighted-sum argmax over a small candidate set**.
4. **Weighted sum cannot guarantee a veto.** No finite weight is a guaranteed veto, but
   irreversible/destructive/over-hard-budget actions need a true constraint. Add a
   **hard-veto filter** stage on the candidate set *before* fusion. Irreversible actions
   also get a **confirmation gate**.
5. **"Complete is permanent" is too rigid.** A finished goal re-opens on user follow-up.
   Objectives use **re-armable** completion.
6. **In-process modules lose MOOS's crash isolation.** Reintroduce isolation deliberately
   exactly where risk lives: tools and sub-agents run as **subprocesses with timeout +
   sandbox**.
7. **An LLM-judge scorer is expensive.** You cannot sample a large action space. Most
   objectives are **cheap heuristics**; at most one is LLM-backed, and it scores only a
   **small LLM-proposed candidate shortlist**.
8. **Plateau thrash (from ZAIC_LEQ/HEQ).** A pure constraint ("stay under budget") assigns
   equal utility to everything within the limit, so the choice within-budget is arbitrary
   and can oscillate. Constraint-shaped objectives **must be paired with a preference
   objective** (e.g. `make_progress`) that breaks ties.

---

## 3. Components

### 3.1 Blackboard — the "session"
Thread-safe map `key → Entry{ value: JSON, source, source_aux, timestamp, seq }`,
latest-value.

- `post(key, value, source, aux?)` — write, notify matching subscribers, append to Eventlog.
- `subscribe(key_or_pattern, handler, min_interval)` — wildcard patterns; `min_interval`
  debounces expensive reactions.
- **Single JSON payload type.** Drop MOOS's STRING/DOUBLE split and the "type fixed on
  first write, wrong-type write silently ignored" quirk — validate explicitly instead.
- Subscriptions are **idempotent** and re-asserted on `on_attach()` (the
  RegisterVariables-on-reconnect discipline).
- One Blackboard instance = one session ("community").

### 3.2 Eventlog — the transcript
Append-only `time | key | source | value` log (the `.alog` analog). Backs:
agent memory/history, replay, and automated evals/regression (grep-to-grade). The fix for
the latest-only blackboard.

### 3.3 Module — base class (the "app")
Lifecycle, dispatched by the runtime:
- `on_start(config)` — parse typed config (schema-validated), register interests.
  A bad/unknown key raises a **surfaced** config warning (not silent), and a fatal one
  fails the launch with a **visible** message (MALCONFIG), never a silent degrade.
- `on_attach()` — idempotent (re-)subscribe; safe to re-run on reconnect.
- `on_event(delta)` — **primary path**; react to changes on subscribed keys.
- `tick()` — slow housekeeping only (timeouts, budget decay, watchdog).
- `build_report()` — structured status for observability.

Modules never call each other directly — only via the blackboard. Core modules run as
threads. Concrete:

- **LLMModule** — calls the model (Anthropic Claude over HTTPS). Provider behind an
  interface so others can be added. Proposes next actions / generates text.
- **ToolRunner** — owns the tool registry; spawns tool subprocesses (native JSON-lines or
  via the MCP adapter), enforces **timeout + sandbox (rlimit)**, posts results. Tracks and
  reaps every child.
- **MemoryModule** *(post-MVP)* — retrieval over the Eventlog / external store.
- **Arbiter** — the helm (§3.5).
- **ChatModule** — the user's "chat-app interface": bridges a front-end to the blackboard
  (`USER_MESSAGE` in → `ASSISTANT_MESSAGE` out). MVP form is stdin/stdout.

### 3.4 Objective — the "behavior"
A pluggable, config-gated, weighted decision-influencer.

- `active(ctx) → bool` — run-conditions (`condition=` analog). An objective can be active
  yet have no opinion this turn (the Running-vs-Active distinction — do not treat a silent
  objective as a failure).
- `score(ctx, candidates) → map<action, utility>` — the IvP-function analog over a
  **discrete** candidate set. (Used by arbiter policy v2.)
- `veto(ctx, action) → bool` — the hard constraint the weighted sum cannot express.
- `flags` on lifecycle transitions — blackboard side-effects that sequence objectives
  across turns (objectives coordinate only *between* turns, never within one — a clean,
  debuggable concurrency model; also satisfies the immutability rule).
- `weight` — priority (dominate, not veto).
- **Re-armable** completion.

Examples: `answer_user` (preference, high weight), `stay_on_budget` (soft decay near a
cap + hard `veto` at the cap), `verify_claim`, `avoid_destructive` (veto +
confirmation-gate), `make_progress` (anti-loop tie-breaker).

### 3.5 Arbiter — the helm (pluggable policy)
Per-turn, event-driven loop:

1. Build an **immutable context snapshot** from the blackboard, plus a "changed since last
   turn" delta (the info-buffer analog). Built once; passed by reference to every
   objective so all reason over identical context. Never mutated mid-turn.
2. Recompute **MODE / phase** — `PLANNING / EXECUTING / VERIFYING / AWAITING_USER` —
   *derived* from blackboard predicates, exposed as a string objectives gate on. The mode
   is never written directly; an operator/user override feeds the derivation as an input
   predicate.
3. Gather **candidate actions** (LLM proposes + cheap heuristics).
4. **Hard-veto filter** — drop any candidate any objective vetoes (true constraint stage,
   before fusion).
5. **Policy (pluggable):**
   - **v1 (default):** the LLM's proposed action wins among survivors; objectives act as
     gates/vetoes.
   - **v2 (the experiment):** objectives `score()` the survivors; arbiter picks
     `argmax_a Σ weight_i · score_i(a)`.
6. Post an explicit **NEXT_ACTION** every turn — always definite, including an explicit
   `WAIT`/no-op, so a stalled agent is detectable (the "decision vars are never stale"
   idiom) without re-issuing a side-effecting call (which would loop/burn tokens).

Carries last turn's action as a **weak prior** for stability (not a lock-in). On
all-stop, emits a reason (`NothingToDo` → stop and ask the user; `MissingDecVars`;
`BehaviorError`/safety).

### 3.6 Launcher — pAntler
Reads the Manifest, brings up the **Blackboard first**, then instantiates modules and
objectives (staggered, dependency-ordered) and wires them to the blackboard. **Fail-fast
with a visible error** on bad config. Owns lifecycle and a **kill-all path that reaps
every tool/sub-agent subprocess** — no orphans (the ktm/zkill discipline → explicit
lifecycle ownership). The launcher does **not** participate in the decision loop (init is
separate from runtime).

### 3.7 Observability
- **status** — every module's `build_report()` aggregated by a monitor (TUI post-MVP;
  CLI dump in MVP). Critical because agent failures are silent/semantic — looping,
  drifting off task, hallucinating — not crashes.
- **scope** — live blackboard inspector (dump/stream keys).
- **poke** — human intervention channel: inject `INTERRUPT=true`,
  `USER_CLARIFICATION=…`, or approve a gated action. Human-in-the-loop steering.
- **Eventlog replay/eval** — offline grep/filter to debug and grade runs.
- **PARK/DRIVE pause** — toggled by an override key; a paused agent keeps **ingesting**
  events (so it has fresh context on resume) but takes **zero actions**. Multiple override
  inputs resolve by a defined precedence (latest-wins).

---

## 4. Manifest (the `.moos` analog) — plain-text MOOS-style blocks

The manifest is plain text in the MOOS `.moos` style — `#` comments and
`Section = name { key = value }` blocks — parsed by hades itself, with **no TOML/JSON
config dependency**. This keeps hades faithful to MOOS, where the mission file is exactly
this format, and matches the "just a text config, Linux style" requirement.

```ini
# manifests/dev.hades — plain text, MOOS-style blocks

Session
{
  name  = hades-dev
  model = claude-opus-4-8          # provider behind an interface; Anthropic default
}

Module = llm
Module = tool_runner
Module = chat

Tool = fs    { mcp    = npx @modelcontextprotocol/server-filesystem /work }
Tool = shell { native = ./tools/shell-tool }

Arbiter { policy = v1 }            # swap to v2 for the experiment

Objective = answer_user        { weight = 100 }
Objective = stay_on_budget     { weight = 60  hard_cap_usd = 1.0 }
Objective = avoid_destructive  { veto = true }
```

The parser reuses the MOOS `GetConfiguration` idiom: read a block's lines, split each on
the first `=`, lower-case the key, and dispatch through `set*OnString`-style validators
(bounds/type checks, one line each). An **unknown key → a surfaced warning** (not silent);
a malformed/unrecognized block → **visible MALCONFIG**, never a silent default. Values that
legitimately contain spaces (an MCP command line, a short prompt) keep their internal
whitespace (the preserve-space variant). A long system prompt is given as a file reference
(`system_prompt_file = …`) rather than inlined — the one place MOOS's flat `key = value`
style is too thin, handled by indirection instead of a nested data format.

---

## 5. Tool protocol

Two paths into one internal tool registry on the blackboard:

- **Native** — a lean hades tool protocol over a subprocess: JSON-lines on stdin/stdout
  (`{"call": "...", "args": {...}}` → `{"ok": true, "result": ...}`). First-party tools.
- **MCP adapter** — a module that wraps any MCP server (stdio JSON-RPC) and exposes its
  tools through the same internal interface. Gives access to the MCP ecosystem,
  language-agnostic.

Both feed the same registry; the LLM and arbiter see one uniform tool interface. Every
tool runs as an isolated subprocess with timeout + sandbox.

---

## 6. Technology

- **C++20**, **CMake**, built inside a **Nix dev shell** (`flake.nix`, pinned
  `nixos-26.05`; `nix develop`) — reproducible toolchain, no ad-hoc system packages.
- **cpr** (C++ Requests, libcurl-backed) — HTTPS to the Anthropic API, including SSE token
  streaming. Provider behind an interface so other backends can be added.
- **nlohmann/json** — LLM message bodies + MCP JSON-RPC (not the manifest).
- **Plain-text MOOS-style manifest**, parsed by hades itself — no TOML/JSON config dep.
- **std::thread + condition_variable** for the blackboard and module threads (introduce an
  async runtime such as asio only if a concrete need appears).
- **posix_spawn + pipes + rlimit** for tool/sub-agent subprocesses.
- **GoogleTest** — TDD per project standard.

Libraries-first to move fast; replace with custom code once the problem is understood.

---

## 7. Source layout (many small files)

```
hades/
  flake.nix  flake.lock  .gitignore
  CMakeLists.txt
  include/hades/   blackboard.h module.h objective.h arbiter.h launcher.h eventlog.h
                   config.h llm/provider.h tool/registry.h
  src/core/        blackboard.cpp eventlog.cpp launcher.cpp
  src/config/      manifest.cpp        # MOOS-style plain-text block parser
  src/module/      llm_module.cpp tool_runner.cpp memory_module.cpp chat_module.cpp
  src/arbiter/     arbiter.cpp policy_v1.cpp policy_v2.cpp mode.cpp
  src/objective/   answer_user.cpp stay_on_budget.cpp verify_claim.cpp avoid_destructive.cpp
  src/llm/         anthropic_provider.cpp
  src/tool/        native_tool.cpp mcp_adapter.cpp subprocess.cpp
  src/obs/         status.cpp scope.cpp poke.cpp
  manifests/       dev.hades
  tests/
```

---

## 8. MVP — first vertical slice

A working chat agent end-to-end that proves the architecture (pub/sub + module lifecycle +
pluggable arbiter + isolated tool + real Claude loop):

1. **Blackboard + Eventlog + Module base + Launcher + manifest parse.**
2. **LLMModule** → one real Anthropic Claude call.
3. **ToolRunner** with ONE native tool (e.g. read a file / run a bounded shell command) +
   an MCP-adapter stub.
4. **Arbiter v1** — LLM decides; `avoid_destructive` (veto + confirmation gate) and
   `stay_on_budget` objectives active.
5. **ChatModule** = stdin/stdout — talk to the agent in the terminal.
6. **Observability** — Eventlog to disk + a `scope` CLI.

**Deferred (post-MVP):** v2 scoring arbiter (the research experiment), MemoryModule
retrieval, sub-agent fan-out, the monitor TUI, richer MCP features, and the full phase
state-machine (MVP ships a stub phase).

---

## 9. Success criteria

- A user can write a manifest, run the launcher, and **chat with a Claude-backed agent**
  in the terminal that can call at least one tool.
- A destructive/over-budget action is **vetoed or gated**, demonstrably, not merely
  down-weighted.
- The full session is **replayable** from the Eventlog.
- Adding a new tool or objective requires **only a manifest change + a plugin**, no change
  to the core.
- The arbiter policy is **swappable** (v1 ↔ v2) without touching modules.
