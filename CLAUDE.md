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
| pAntler | Launcher |
| .moos mission | Manifest (plain-text MOOS-style blocks, NOT TOML) |
| .alog / alogview | Eventlog / `hades-scope` |
| **a vehicle/community** | **one agent = Blackboard + Arbiter + modules** |
| pShare / pMOOSBridge | (planned) a **Bridge** module between Blackboards for agent↔agent |

**Personas/multi-agent:** 1 agent = 1 community (Blackboard+Arbiter+modules). Objectives are that
agent's goals, NOT other agents. More agents = replicate the community; bridge them with a `pShare`-style
Bridge. Levels: (1) separate manifests [today], (2) `/persona` switch, (3) a `Community` struct ×N +
router + Bridge [real multi-agent].

## Current state (2026-06-29)
`main` @ `00f38cc`, **85/85 tests**, ~9 MB RSS, **live** against PPQ (`claude-haiku-4.5`).
Built: Blackboard+Eventlog · Arbiter v1 (veto/confirm gate, max-steps guard) · **6 tools**
(`fs_read shell write_file list_dir http_fetch save_memory`, self-describing) · safety gate (destructive
shell + write_file → y/N; `save_memory` NOT gated — append-only to own store) · layered **system prompt**
(SOUL/USER static layers) · **dynamic MemoryModule** (see below) · two front-ends: **stdin REPL** and
**HTTP** (`--serve`, loopback; POST /chat, POST /confirm, GET /health).

### MemoryModule (shipped 2026-06-29) — dynamic memory
Tool writes, app reads, one shared JSONL file store. **`save_memory`** native tool (append-only to
`.hades/memory.jsonl`) the LLM calls to persist a fact. **MemoryModule** (`type()=="memory"`) subscribes
`USER_MESSAGE` → `load_memories()` → `rank_memories()` (pure keyword top-N; **v2 seam = embeddings**) →
posts `RETRIEVED_MEMORY`. Arbiter `start_turn()` splices it as an **ephemeral** `{role:system}` "Relevant
memories:" block immediately before the last user msg — never stored in `history_` (refreshes each turn).
Pieces: `src/memory/{rank,store}.cpp`, `src/module/memory_module.cpp`, `tools/save_memory_main.cpp`.
Config: `Memory { store=… top_n=… }` block (MUST be multi-line — parser is one-kv-per-line). Wiring
registers MemoryModule **before** the Arbiter (single-thread pump order → `RETRIEVED_MEMORY` fresh same
turn) and appends the store path to the `save_memory` tool argv (single source of truth).
**Not yet live-smoke-tested against PPQ** (needs key) — unit/integration green (85/85).

## Build / run
```bash
export HADES_API_KEY=<key>                                   # key never in the manifest
nix develop --command cmake -S . -B build -G Ninja           # configure (once)
nix develop --command cmake --build build                    # build
nix develop --command ctest --test-dir build                 # test (85/85)
nix develop --command ./build/hades manifests/dev.hades             # chat REPL
nix develop --command ./build/hades manifests/dev.hades --serve 8080  # HTTP server
nix develop --command ./build/hades-scope session.log              # replay (key redacted)
```
Targets: `hades_core` (lib), `hades` (app), `hades-{fs-read,shell,write-file,list-dir,http-fetch,save-memory}` (tools),
`hades-scope` (CLI), `hades_tests`. Stack: libcpr, nlohmann_json, **httplib** (nixpkgs attr `httplib`),
gtest, std::thread. Manifest: `manifests/dev.hades`. Persona: `prompts/soul.md`.

## How it's built (process)
Spec → plan → TDD, on feature branches merged ff to `main`. Specs/plans in `docs/superpowers/`;
SDD ledger + per-task reports in `.superpowers/sdd/` (gitignored). Every change: build + `ctest` green
inside `nix develop` before commit. Reviews via the `cpp-reviewer` agent.

## NEXT possible work for MemoryModule (v2)
embeddings/vector retrieval (drop in behind `rank_memories` — the seam is built) · auto-extract memories
per turn (LLM-summarized, vs today's explicit `save_memory`) · dedup / decay / importance · tags ·
sqlite backend · edit/delete/forget · live-smoke against PPQ (save → restart → retrieve).

## Other open work
SSE/WebSocket streaming · session resume (history is in-memory only) · MCP tool discovery (MCP servers
can be called but aren't announced to the LLM) · make `Module =` manifest blocks actually drive the
module set (currently ignored — binary hard-codes modules) · persona switch · prompt caching.

## Gotchas
- nixpkgs renamed `cpr`→`libcpr` and cpp-httplib's attr is **`httplib`**.
- The manifest `Module =` lines are **decorative** today (binary hard-codes its modules).
- API key: env var only, redacted in the Eventlog; never put it in the manifest.
- Single-threaded bus — the HTTP server serializes all turns under one mutex.
- **Manifest parser is one-kv-per-line.** A single-line block with two `k = v` pairs mis-parses (first
  `=` wins, rest swallowed into the value). Multi-line blocks only (like `Session`/`Memory`). This bit us:
  `Memory { store=… top_n=… }` on one line silently broke retrieval; caught by the final whole-branch review.
- `save_memory` store path must contain **no whitespace** (tool argv is whitespace-split) — wiring
  throws `MalConfig` if it does.
