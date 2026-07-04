# src reorganization: apps / behaviors / core — design

**Date:** 2026-07-04
**Status:** Approved (Vaios — "proceed with the reorg")
**Branch:** `refactor/src-apps` (off `main` @ `43e31ef`)

## Problem

The MOOS-IvP mapping (Module = app, Objective = behavior) lives only in CLAUDE.md — the source
tree doesn't show it. 45 `.cpp` files across 13 flat-ish directories; helper files sit far from
the app that owns them (e.g. `src/telegram/parse.cpp` vs `src/module/telegram_module.cpp`).
Vaios's backlog items 1+2: fewer files, organized as close as possible to MOOS-IvP apps, with
behaviors-vs-apps visible in the layout.

## Decisions (Vaios, 2026-07-04)

| Question | Decision |
|---|---|
| Scope | **src/ + CMake only.** `include/hades/**`, `tests/`, `app/`, `tools/`, `web/` untouched — zero include-line churn outside src, tests unchanged. |
| Merging | **1–2 files per app.** One `.cpp` per app dir (module + its helpers); split only where it would pass ~500 lines (embedding_memory → app + providers). 45 → 22 files. Headers/pure seams unchanged — unit tests still pin the helpers. |
| Naming | **Plain names.** `src/apps/<name>/`, `src/behaviors/`, `src/core/`. No MOOS prefixes. |

MOOS-IvP grounding: MOOS itself uses one **directory** per app (`ivp/src/pHelmIvP/`) and
`lib_*` dirs for shared infrastructure and behavior libraries. `src/core/` = the lib analog,
`src/apps/<name>/` = per-app dirs, `src/behaviors/` = the behavior library.

## Target tree (45 → 22 files) and full mapping

```
src/
  core/                        # shared infrastructure (MOOS lib_* analog — NOT apps)
    blackboard.cpp             ← src/core/blackboard.cpp                    (move)
    eventlog.cpp               ← src/core/eventlog.cpp                      (move)
    executor.cpp               ← src/core/executor.cpp                      (move)
    launcher.cpp               ← src/core/launcher.cpp                      (move)
    session.cpp                ← src/core/session_id.cpp + src/core/session_history.cpp   (merge, 89)
    config.cpp                 ← src/config/manifest.cpp + src/config/prompt.cpp
                                 + src/core/version.cpp                     (merge, ~221)
    subprocess.cpp             ← src/tool/subprocess.cpp                    (move — shared by
                                 ToolRunner, registry warm, hades-shell binary; genuinely core)
  apps/                        # one dir per Module (MOOS one-dir-per-app)
    arbiter/arbiter.cpp        ← src/arbiter/arbiter.cpp                    (move, 352)
    llm/llm.cpp                ← src/module/llm_module.cpp + src/llm/openai_compat_provider.cpp
                                 + src/llm/http_cpr.cpp                     (merge, 208)
    tool_runner/tool_runner.cpp← src/module/tool_runner.cpp + src/tool/registry.cpp
                                 + src/tool/mcp_adapter.cpp                 (merge, 224)
    memory/memory.cpp          ← src/module/memory_module.cpp + src/memory/rank.cpp
                                 + src/memory/store.cpp                     (merge, 96)
    embedding_memory/embedding_memory.cpp
                               ← src/module/embedding_memory_module.cpp + src/embedding/vector_cache.cpp
                                 + src/embedding/indexer.cpp + src/embedding/session_turns.cpp
                                 + src/embedding/vec_math.cpp               (merge, 343)
    embedding_memory/providers.cpp
                               ← src/embedding/http_embedding_provider.cpp
                                 + src/embedding/subprocess_embedding_provider.cpp
                                 + src/embedding/persistent_child.cpp       (merge, 172)
    skills/skills.cpp          ← src/module/skills_module.cpp + src/skills/scan.cpp   (merge, 108)
    chat/chat.cpp              ← src/module/chat_module.cpp                 (move, 197)
    serve/serve.cpp            ← src/module/http_server_module.cpp + src/config/serve_config.cpp
                                                                            (merge, 204)
    telegram/telegram.cpp      ← src/module/telegram_module.cpp + src/telegram/parse.cpp
                                 + src/telegram/cpr_telegram_api.cpp        (merge, 321)
    bridge/bridge.cpp          ← src/module/bridge_module.cpp + src/bridge/protocol.cpp
                                 + src/bridge/cpr_bridge_http.cpp           (merge, 354)
    scope/scope.cpp            ← src/obs/scope.cpp                          (move)
    scope/scope_main.cpp       ← src/obs/scope_main.cpp                     (move — the
                                 hades-scope CLI, alogview analog; a binary = an app)
  behaviors/                   # Objectives — ONE agent's competing helm goals
    standard_behaviors.cpp     ← src/objective/stay_on_budget.cpp + src/objective/avoid_destructive.cpp
                                 + src/objective/peer_loop_guard.cpp        (merge, 56)
    capability_policy.cpp      ← src/objective/capability_policy.cpp        (move, 262)
```

Old directories (`src/config`, `src/llm`, `src/module`, `src/objective`, `src/memory`,
`src/embedding`, `src/skills`, `src/telegram`, `src/bridge`, `src/tool`, `src/obs`,
`src/arbiter`) are removed when emptied. Every merged file ≤ 354 lines except
`embedding_memory.cpp` (343) — all within house norms (200–400 typical).

## Merge rules (each merged file)

1. **New file header** (house style): `// path — one-line purpose` + a short block naming the
   merged parts (e.g. "telegram front-end app: module + Bot-API parse/builders + cpr shell").
2. **Include union, deduped**, in house order. Section comments between the merged parts
   (`// ── parse helpers (was src/telegram/parse.cpp) ──`) so the file stays navigable and
   history stays findable (`git log --follow` breaks on merges; the was-comment + pickaxe cover it).
3. **File-local helper collisions:** identical duplicates → keep ONE copy (known concrete case:
   `static split_ws` in BOTH `tool_runner.cpp` and `registry.cpp` — dedupe to one). Different
   helpers sharing a name → rename the narrower one. Anonymous-namespace contents merge into one
   `namespace { }` per file where practical. This is the ONE real hazard class of the whole
   refactor; per-task review checks it explicitly.
4. **Zero behavior change.** No signature, type, key, or namespace changes. Public headers in
   `include/hades/**` untouched → the test suite compiles unmodified and must stay green.
5. Pure moves use `git mv` (history preserved); merges create the new file and delete the olds.

## CMake

`CMakeLists.txt` `target_sources(hades_core …)` lines rewritten to the new 20 lib paths (22
files minus the 2 scope files, which belong to the `hades-scope` executable — its
`add_executable(hades-scope src/obs/scope_main.cpp)` + `target_sources` line move to
`src/apps/scope/`). Tool binaries, test sources, compile definitions unchanged.

## Docs policy

- **CLAUDE.md:** do NOT rewrite historical `Pieces:` paths in feature sections. Add ONE Gotcha
  line: "src reorganized 2026-07-04: `src/apps/<name>/` (one dir per Module) + `src/behaviors/`
  (Objectives) + `src/core/` (infra); `Pieces:` paths in older sections are pre-reorg." Update
  only the always-current bits (the `## The one idea`/current-state layout mentions if any).
- **docs/manifest-reference.md:** IS a live operator doc — its 13 `src/**` section citations get
  updated to the new paths (list already enumerated by the accuracy review).

## Process

Branch `refactor/src-apps`. One task per target area (core, behaviors, then each app), each task:
move/merge → CMake update → **full build + 351/351 suite** → commit. TSan lane (`build-tsan`)
re-run at the end (full suite) — merges can't introduce races, but the gate is cheap and the
suite is the contract. Final whole-branch review before merge (per project process).

## Non-goals

No renames of types/functions/bus keys/manifest keys · no header moves · no test edits · no
behavior change of any kind · no `app/` (agent_wiring/hades_main) restructuring · tools/ stays
(tools are actions, not apps — ToolRunner is the app).
