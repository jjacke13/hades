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
| MOOS app | Module (LLMModule, ToolRunner, ChatModule, HttpServerModule, Arbiter) |
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
`main` @ `f6374ed`, **64/64 tests**, ~9 MB RSS, **live** against PPQ (`claude-haiku-4.5`).
Built: Blackboard+Eventlog · Arbiter v1 (veto/confirm gate, max-steps guard) · **5 tools**
(`fs_read shell write_file list_dir http_fetch`, self-describing) · safety gate (destructive shell +
write_file → y/N) · layered **system prompt** (SOUL/USER/MEMORY files, currently STATIC) · two
front-ends: **stdin REPL** and **HTTP** (`--serve`, loopback; POST /chat, POST /confirm, GET /health).

## Build / run
```bash
export HADES_API_KEY=<key>                                   # key never in the manifest
nix develop --command cmake -S . -B build -G Ninja           # configure (once)
nix develop --command cmake --build build                    # build
nix develop --command ctest --test-dir build                 # test (64/64)
nix develop --command ./build/hades manifests/dev.hades             # chat REPL
nix develop --command ./build/hades manifests/dev.hades --serve 8080  # HTTP server
nix develop --command ./build/hades-scope session.log              # replay (key redacted)
```
Targets: `hades_core` (lib), `hades` (app), `hades-{fs-read,shell,write-file,list-dir,http-fetch}` (tools),
`hades-scope` (CLI), `hades_tests`. Stack: libcpr, nlohmann_json, **httplib** (nixpkgs attr `httplib`),
gtest, std::thread. Manifest: `manifests/dev.hades`. Persona: `prompts/soul.md`.

## How it's built (process)
Spec → plan → TDD, on feature branches merged ff to `main`. Specs/plans in `docs/superpowers/`;
SDD ledger + per-task reports in `.superpowers/sdd/` (gitignored). Every change: build + `ctest` green
inside `nix develop` before commit. Reviews via the `cpp-reviewer` agent.

## NEXT TASK: a MEMORY app for the agent
Build a **MemoryModule** = a MOOS-app-style module (NOT an Objective, NOT inside the Arbiter) that gives
the agent **memory**. Today MEMORY is a STATIC file (system-prompt layer). Want **dynamic**: persist
facts/observations + retrieve the relevant slice per turn (mem0/MemGPT-style RAG). Likely shape (to
brainstorm): subscribe to USER_MESSAGE/ASSISTANT_MESSAGE (+ a SAVE_MEMORY key), write to a store, post
RETRIEVED_MEMORY the Arbiter folds into context. Open design choices: store (file/sqlite/vector),
retrieval (keyword vs embeddings), write trigger (auto vs explicit), delivery (system-prompt layer vs
context block).

## Other open work
SSE/WebSocket streaming · session resume (history is in-memory only) · MCP tool discovery (MCP servers
can be called but aren't announced to the LLM) · make `Module =` manifest blocks actually drive the
module set (currently ignored — binary hard-codes modules) · persona switch · prompt caching.

## Gotchas
- nixpkgs renamed `cpr`→`libcpr` and cpp-httplib's attr is **`httplib`**.
- The manifest `Module =` lines are **decorative** today (binary hard-codes its modules).
- API key: env var only, redacted in the Eventlog; never put it in the manifest.
- Single-threaded bus — the HTTP server serializes all turns under one mutex.
