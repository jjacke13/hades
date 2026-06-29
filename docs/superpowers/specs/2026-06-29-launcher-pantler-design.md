# Launcher / pAntler — make the manifest drive the module set

**Date:** 2026-06-29
**Status:** approved (brainstorm via Q&A) → ready for plan

## Goal

Make the manifest's `Module =` lines **actually drive which modules run** — the harness's one dishonest
extensibility claim (the `Launcher` is built + tested but **unused** on the live path; the binary
hard-codes its modules in `agent_wiring`). This completes the `.moos`-mission-file analogy: the manifest
is the mission file, `Module =` lines are the **Antler roster**, and the `Launcher` is **pAntler** that
launches them.

**Approach: pragmatic pAntler** (chosen over a full bus-decoupled refactor). The Launcher does
**roster-driven instantiation + type validation**; the small amount of cross-wiring the modules need
(the Arbiter needs the ToolRunner's tool specs, objectives, model, prompts) stays **explicit** in
`agent_wiring`, but now **guarded on which modules the roster actually includes**.

## Why config stays out of the Module blocks

In hades, a module's config does NOT live in its `Module =` block — it lives elsewhere: the LLM reads the
`Session` block, the MemoryModule reads the `Memory` block, the ToolRunner reads all `Tool =` blocks, the
Arbiter reads the `Objective =` blocks + `Session`. So a generic `on_start(own_block)` lifecycle does not
fit. We therefore use the Launcher for the **roster** (presence + order + validation) and keep the
existing per-module configuration + cross-wiring in `agent_wiring`, applied only to modules the roster
includes. (A fully bus-decoupled config — modules self-configure from the manifest, Arbiter reads tool
specs off the bus — is the purer end state, deferred; it pairs with the Arbiter-decomposition.)

## Components

### 1. `Launcher` gains roster instantiation + ownership transfer — `include/hades/launcher.h`, `src/core/launcher.cpp`

Keep the existing `register_factory`, `build`, `modules`, `shutdown`. Add:

```cpp
// Instantiate the Module roster from the manifest's `Module =` lines, in order, via the
// registered factories. Throws MalConfig on an unknown module type. Does NOT call
// on_start/on_attach — the caller (agent_wiring) drives lifecycle + cross-wiring. Roster
// validation only; modules are owned by the Launcher until taken.
void instantiate(const Manifest& m);
bool has(const std::string& type) const;                       // a module of this type was instantiated
std::unique_ptr<Module> take(const std::string& type);          // transfer ownership out (nullptr if absent)
```

`instantiate` mirrors `build`'s factory lookup + MalConfig-on-unknown, but only creates + stores (no
lifecycle). `take(type)` removes and returns the first module whose `type()` matches (the factory key
equals the module's `type()` for all hades modules), so `agent_wiring` can move instances into the
typed `Agent` members. `build()` is retained unchanged for the generic lifecycle contract (still tested).

### 2. `agent_wiring` uses the Launcher on the live path — `app/agent_wiring.{h,cpp}`

- Split the current `build_agent_impl` into a shared **`wire_agent(Agent&, bb, session, tools, objectives, memory, model)`** that performs config + cross-wire + attach on whatever `Agent` members are **present** (null-guarded throughout). The ordering stays dependency-correct (ToolRunner warmed → Arbiter gets specs; MemoryModule attached **before** Arbiter), enforced by the call sequence — **not** by roster order (so a mis-ordered roster can't silently stale the memory).
- **Manifest overload** `build_agent(bb, m)`:
  1. Create a `Launcher`, register the six factories (`llm`, `tool_runner`, `memory`, `arbiter`, `chat`, `serve`) — each just constructs its module (the `llm` factory makes a bare `LLMModule()`; it self-builds its provider from the `Session` block in `on_start`, the existing config path).
  2. `launcher.instantiate(m)` — **validates the roster** (MalConfig on unknown type).
  3. Move each present instance into the `Agent`'s typed member via `take()` (absent → the member stays `nullptr`).
  4. `wire_agent(...)` configures/attaches only the present modules.
- **Test overload** `build_agent(bb, provider, tools, objectives, model, memory, session)`: unchanged in spirit — builds all six modules directly into the `Agent` (injecting the provider), then `wire_agent(...)`. No Launcher (tests pass explicit block lists + an injected provider, not a `Module =` roster).
- `Agent` members may now be `nullptr` (a roster omitting a module). No struct change needed; members are already `unique_ptr` and default-null.

### 3. Binary null-guards — `app/hades_main.cpp`

`agent.serve` / `agent.chat` may be null (roster omitted them). Guard:
- serve mode: `if (serve && agent.serve) agent.serve->listen(...)` — else a clear message ("no `serve` module in the manifest's Module roster").
- REPL mode: `if (agent.chat) agent.chat->run_repl(...)` — else a clear message.

### 4. Manifest roster completed — `manifests/dev.hades`

The live agent uses memory + the web UI, which are currently hard-coded but absent from the roster. Add
them (memory **before** arbiter for clarity, though wiring enforces order):
```
Module = llm
Module = tool_runner
Module = memory
Module = chat
Module = arbiter
Module = serve
```

## Data flow

```
build_agent(bb, manifest)
  └─ Launcher: register factories → instantiate(Module= roster)  [MalConfig on unknown type]
       └─ take() present instances into Agent members (absent → null)
            └─ wire_agent(): config + cross-wire + attach, guarded on presence, dependency-ordered
```

## Error handling

- Unknown `Module =` type → `MalConfig` (fail fast, names the bad type). This is the new validation win.
- Roster omits an optional module (e.g. `serve`) → that member is null; the agent runs without it; the
  binary reports clearly if asked to use a missing module (`--serve` with no `serve` in the roster).
- Roster omits a **required** module (e.g. `arbiter`) → the agent can't run a turn; `wire_agent` proceeds
  but the binary/REPL will simply produce no assistant turn. (v1: no hard requirement enforcement beyond
  null-guards; a future "required modules" check is a noted follow-up.)

## Testing (TDD, GoogleTest)

- `test_launcher` (extend) — `instantiate`: builds the roster in order; `has`/`take` find by type; `take`
  transfers ownership (second `take` → nullptr); **unknown Module type → MalConfig**.
- `test_pantler_wiring` (new) — `build_agent(bb, manifest)` with:
  - a full roster → all six members non-null; a `USER_MESSAGE` drives a turn (scripted provider) end-to-end.
  - a roster **omitting `serve`** → `agent.serve == nullptr`, the rest still work (proves the roster drives presence).
  - an **unknown** `Module = bogus` → `EXPECT_THROW(build_agent(bb, m), MalConfig)`.
- Existing `test_e2e`, `test_memory_wiring`, `test_pin_fact_wiring`, `test_serve` stay green — they use the
  test overload (direct build, no Launcher) and are unaffected. The shipped `dev.hades` resolves to all six
  modules (lock via the e2e/manifest tests).

## Out of scope (follow-ups)

- Bus-decoupled config (modules self-configure from the manifest; Arbiter reads tool specs off the bus) —
  the purer pAntler; deferred, pairs with Arbiter-decomposition.
- "Required modules" enforcement (error if `arbiter`/`llm` absent) — noted; v1 relies on null-guards.
- Roster-driven **attach order** (let the manifest set pump order, MOOS-Antler-style) — deferred; v1
  enforces dependency order in `wire_agent` to avoid the silent-stale-memory footgun.

## MOOS-IvP mapping

`Launcher` = **pAntler**; the manifest = the **`.moos` mission file**; `Module =` lines = the **Antler
`Run =` roster**; the per-section blocks (`Session`/`Tool`/`Objective`/`Memory`/`Serve`) = **ProcessConfig**.
After this change the analogy is honest: pAntler launches exactly the apps the mission file lists.
