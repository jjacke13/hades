# hades — project notes (CLAUDE.md)

**hades** = an AI-agent harness in **C++20** that ports the **MOOS-IvP** robotics architecture to a
software, LLM-driven agent. Own git repo, branch `main`, no remote. Build/run only inside the Nix
dev shell (`nix develop`).

## The one idea
A central **Blackboard** (pub/sub: latest-value map + FIFO `pump()`) that modules talk through —
no module calls another directly. The **Arbiter** (the "helm") runs the per-turn loop: ask the LLM →
gate the proposed action through **Objectives** (veto/confirm) → run a tool or answer → loop tool
results back. Tools run as **isolated subprocesses**. See `docs/architecture.md` (Mermaid diagrams).

## MOOS-IvP ↔ hades
| MOOS-IvP | hades |
|---|---|
| MOOSDB | Blackboard (+ append-only Eventlog for history) |
| MOOS app | Module (LLMModule, ToolRunner, ChatModule, HttpServerModule, MemoryModule, Arbiter) |
| pHelmIvP (helm) | Arbiter (v1: LLM decides, objectives gate) |
| behavior | Objective (stay_on_budget, avoid_destructive) — competing goals of ONE agent |
| pAntler | Launcher (reads `Module=` roster, instantiates the module set) |
| .moos mission | Manifest (plain-text MOOS-style blocks, NOT TOML) |
| .alog / alogview | Eventlog / `hades-scope` |
| **a vehicle/community** | **one agent = Blackboard + Arbiter + modules** |
| pShare / pMOOSBridge | (planned) a **Bridge** module between Blackboards for agent↔agent |

**Personas/multi-agent:** 1 agent = 1 community (Blackboard+Arbiter+modules). Objectives are that
agent's goals, NOT other agents. More agents = replicate the community; bridge them with a `pShare`-style
Bridge. Levels: (1) separate manifests [today], (2) `/persona` switch, (3) a `Community` struct ×N +
router + Bridge [real multi-agent].

## Current state (2026-06-29)
`main` @ `7bf634c`, **127/127 tests** (ASan+UBSan + **TSan** clean), ~9 MB RSS, **live** against PPQ (`claude-haiku-4.5`).
Built: Blackboard+Eventlog · Arbiter v1 (veto/confirm gate, max-steps guard) · **7 tools**
(`fs_read shell write_file list_dir http_fetch save_memory pin_fact`, self-describing) · safety gate
(destructive shell + write_file → y/N; `save_memory`/`pin_fact` NOT gated — append-only to own files) ·
**two memory layers** (core + archival, see below) · layered **system prompt** (SOUL/USER static +
live core MEMORY) · two front-ends: **stdin REPL** (GNU readline — arrows/history/Ctrl-A/E, colored
labels) and **HTTP `--serve`** (browser web UI + JSON API, see below) · **worker-offload concurrency**
(see below).

### Worker-offload (shipped 2026-06-29) — bus stays single-threaded, blocking LLM call offloaded
The bus is **still single-threaded deterministic** (subscriber handlers run ONLY on the pump thread), but
`Blackboard::post()` is now **thread-safe** and there is a `run_until(pred, timeout_s)` event loop, an
`Executor` worker pool (`src/core/executor.cpp`), and **opt-in** `LLMModule::set_executor()`. When set
(the live Manifest path), the blocking `provider_->complete()` HTTP runs on a worker that `post()`s
`LLM_RESPONSE`/`BUDGET_SPENT_USD` back; `run_until` on the pump thread wakes and pumps it. With no
executor (the test `build_agent` overload) the LLM runs inline → all tests byte-identical. **Load-bearing
teardown:** the `Agent`'s `executor` is the **last** struct member (destroyed first → joins workers while
`llm`/`bb` still alive); `hades_main` declares `Blackboard bb` before `Agent agent`. Front-ends drive a
turn via `run_until` (REPL: `turn_done_` flag; HTTP `collect_`: `got_reply_||pending_confirm_`, 180s).
Pieces: `src/core/{blackboard,executor}.cpp`, `src/module/llm_module.cpp`, `app/agent_wiring.*`,
`tests/test_{blackboard,executor,llm_module,offload_e2e}.cpp`. **KNOWN follow-up (build NEXT, before
SSE/Bridge):** `run_until`'s 180s deadline is fixed-at-entry → a legit long multi-step turn can time out,
leaking a stale worker's `LLM_RESPONSE` into the next turn + racing `spent_` (budget UB). Fix = turn-epoch
on `LLM_REQUEST`/`LLM_RESPONSE` + in-flight guard (or reset deadline on progress).

### Web UI (shipped 2026-06-29) — `--serve` browser front-end
`hades <manifest> --serve [port]` runs `HttpServerModule`: serves static files from `web/` (mounted at
`/`) + the JSON API (`POST /chat`, `POST /confirm`, `GET /health`). Page (`web/{index.html,style.css,app.js}`,
dark terminal theme, plain JS no framework) hits `/chat`, renders user/assistant bubbles, Approve/Deny
for confirm-gated actions. Config: `Serve { host, port, webroot }` block (host default `127.0.0.1`; set
`0.0.0.0` for LAN). `resolve_serve_config()` (`src/config/serve_config.cpp`) resolves it; `--serve` port
optional (overrides block). **Security:** loopback default; **CSRF guard** — `authorize()` pre-routing
seam requires an `X-Hades` header on `POST /chat`+`/confirm` (a cross-origin "simple" request can't add
it without a preflight we never grant), blocking a visited website from driving the loopback agent;
static GET exempt. **No password auth by design** (user's private networking) — the `authorize()` seam
is the one-place add for it later. Seam also set for a future settings UI (`web/settings.html` +
`GET/POST /manifest` — deferred). SSE/WS streaming still deferred (replies arrive whole).

### Two memory layers (MemGPT-style, both agent-writable)

### Two memory layers (MemGPT-style, both agent-writable)
- **Archival / searchable** — `save_memory` tool → `.hades/memory.jsonl` (append-only). MemoryModule
  (`type()=="memory"`) keyword-ranks it each turn (`rank_memories`, pure; **v2 seam = embeddings**) and
  posts `RETRIEVED_MEMORY`; Arbiter injects it as an **ephemeral** `{role:system}` "Relevant memories:"
  block before the last user msg. Config: `Memory { store=… top_n=… }`. **LIVE-VALIDATED** (save→restart→recall).
- **Core / always-on** — `pin_fact` tool → `memory/facts.md` (append-only, newlines stripped, parent dir
  created). The Arbiter **re-reads this file every turn** (`read_memory_layer`) and folds it into the
  **leading** `{role:system}` message (after static SOUL/USER) — live same-session. Config: Session
  `memory_file = memory/facts.md`; wiring **requires memory_file when pin_fact is present** (MalConfig
  fail-fast) and appends the path to the tool argv (single source of truth).
Pieces: `src/memory/{rank,store}.cpp`, `src/module/memory_module.cpp`, `src/config/prompt.cpp`
(`assemble_system_prompt`=SOUL+USER, `read_memory_layer`=live core), `tools/{save_memory,pin_fact}_main.cpp`.

## Build / run
```bash
export HADES_API_KEY=<key>                                   # key never in the manifest
nix develop --command cmake -S . -B build -G Ninja           # configure (once)
nix develop --command cmake --build build                    # build
nix develop --command ctest --test-dir build                 # test (127/127)
nix develop --command ./build/hades manifests/dev.hades --serve      # web UI -> http://localhost:8080/
nix develop --command ./build/hades manifests/dev.hades             # chat REPL
nix develop --command ./build/hades manifests/dev.hades --serve 8080  # HTTP server
nix develop --command ./build/hades-scope session.log              # replay (key redacted)
```
Targets: `hades_core` (lib), `hades` (app), `hades-{fs-read,shell,write-file,list-dir,http-fetch,save-memory,pin-fact}` (tools),
`hades-scope` (CLI), `hades_tests`. Stack: libcpr, nlohmann_json, **httplib** (nixpkgs attr `httplib`),
**readline** (REPL line editing, GPL-3, via pkg-config), gtest, std::thread. Manifest: `manifests/dev.hades`. Persona: `prompts/soul.md`.

## How it's built (process)
Spec → plan → TDD, on feature branches merged ff to `main`. Specs/plans in `docs/superpowers/`;
SDD ledger + per-task reports in `.superpowers/sdd/` (gitignored). Every change: build + `ctest` green
inside `nix develop` before commit. Reviews via the `cpp-reviewer` agent.

## NEXT possible memory work (v2)
**Archival:** embeddings/vector retrieval (drop in behind `rank_memories` — the seam is built) ·
auto-extract per turn (LLM-summarized, vs explicit `save_memory`) · dedup/decay/importance · sqlite.
**Core:** `core_memory_replace`/edit/forget tools (only append today) · size cap / eviction · provenance/audit.

## NEXT possible web work
**SSE/WebSocket streaming** (token-by-token in the web UI — needs provider streaming + Arbiter partial
emits + an SSE endpoint) · **settings UI** (`web/settings.html` + `GET/POST /manifest` to view/edit the
manifest — the static-dir + JSON-API seam is ready) · **auth** (fill in the `authorize()` seam — token/
password) · agent↔agent **Bridge** (pShare-style, needs design — parked).

## Architecture-honesty pass (after expert critique, 2026-06-29)
4 debts flagged. **(1) DONE** — manifest `Module=` roster drives modules (pAntler, above).
**(4) DONE** — worker-offload shipped (single-threaded deterministic bus + blocking LLM call offloaded to
an `Executor` worker that `post()`s back; thread-safe `post()` + `run_until()`; opt-in, 127/127 +
TSan-clean — see the Worker-offload section above). **(2) + (3) SPEC'd + PLANNED, build deferred:**
**(2)** harden the one-kv-per-line manifest parser to fail LOUD (silent mis-parse — bit us 3×) — spec/plan
at `docs/superpowers/{specs,plans}/2026-06-29-manifest-parser-fail-loud*`; **(3)** real tool
**permission/capability** model (today: destructive-pattern blocklist; fs_read/http_fetch ungated) —
spec/plan at `docs/superpowers/{specs,plans}/2026-06-29-tool-capability-model*` (both have OPEN QUESTIONS
flagged in the spec header for the user). **NEXT (decided): build the worker-offload run_until follow-up
first** (turn-epoch + in-flight guard — see the Worker-offload section), it precedes SSE/Bridge; then
(2)/(3) are ready to build from their committed plans.

## Other open work
session resume (history is in-memory only) · MCP tool discovery (MCP servers can be called but aren't
announced to the LLM) · persona switch · prompt caching · SSE streaming · settings UI · agent↔agent Bridge (parked).

## Gotchas
- nixpkgs renamed `cpr`→`libcpr` and cpp-httplib's attr is **`httplib`**.
- The manifest `Module =` lines **drive the module set** (pAntler): `build_agent(Manifest)` →
  `Launcher.instantiate` (MalConfig on unknown type) → `take_as` into the Agent → `wire_agent` (null-guarded,
  dependency order). Omit a module → it's absent (`agent.X==nullptr`); binary errors if `llm`/`arbiter`/the
  requested front-end is missing. Cross-wiring (Arbiter←tools/objectives/model/prompt) stays explicit in
  `wire_agent`. dev.hades roster = llm/tool_runner/memory/chat/arbiter/serve.
- API key: env var only, redacted in the Eventlog; never put it in the manifest.
- Single-threaded **dispatch** — subscriber handlers run ONLY on the pump thread (the determinism
  invariant). `post()` is thread-safe (workers call it); the blocking LLM call is offloaded to an
  `Executor` worker when set. HTTP server still serializes whole turns under one mutex. **Teardown order
  is load-bearing:** `Executor` joined before modules+Blackboard (Agent's `executor` is the last member;
  `bb` declared before `agent` in `hades_main`).
- **Manifest parser is one-kv-per-line.** A single-line block with two `k = v` pairs mis-parses (first
  `=` wins, rest swallowed into the value). Multi-line blocks only (like `Session`/`Memory`/`Serve`). Bit
  us **twice** (final whole-branch review each time): `Memory { store=… top_n=… }` and `Serve { host=… port=… webroot=… }`
  single-line → silent mis-parse. Lock tests parse the shipped `dev.hades` to catch regressions.
- `save_memory`/`pin_fact` store paths must contain **no whitespace** (tool argv is whitespace-split) —
  wiring throws `MalConfig` if they do.
- `pin_fact` tool **requires** `memory_file` in the Session block (wiring throws `MalConfig` otherwise) —
  else pins would write a file the Arbiter never reads (silent drift; caught by the final review).
- Web UI: `webroot` is **cwd-relative** (default `web/`) → run `--serve` from the repo root (warns if the
  dir is missing). The page sends an `X-Hades` header; the `authorize()` CSRF seam requires it on
  `POST /chat`+`/confirm` (don't strip it client-side). `/.hades/` and runtime stores are gitignored.
- Core memory (`memory/facts.md`) is **git-tracked** and the agent mutates it at runtime → expect
  working-tree churn; review/commit the agent's pins as curated standing facts (or gitignore it).
- Interactive REPL uses readline only when stdin is a **real TTY**; piped/test input falls back to
  `std::getline` (keeps the injected-stream test seam). Arrow-key editing verified live 2026-06-29.
