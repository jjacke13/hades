# src Apps/Behaviors/Core Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize `src/` from 45 `.cpp` files into 22, laid out MOOS-IvP-style: `src/core/` (shared infra), `src/apps/<name>/` (one dir per Module), `src/behaviors/` (Objectives) — with ZERO behavior change.

**Architecture:** Pure moves (`git mv`) + TU merges (new file assembled from old files' verbatim content, olds deleted). Public headers in `include/hades/**` are untouched, so `tests/`, `app/`, `tools/` compile unmodified — the 351-test suite is the invariant gate after every task. Spec: `docs/superpowers/specs/2026-07-04-src-apps-reorg-design.md` (committed `f619ca0`).

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, git.

## Global Constraints

- **Every build/test command runs inside `nix develop`**: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. **Expected after EVERY task: 351/351.** A task that ends red is not done.
- Branch `refactor/src-apps` (spec committed `f619ca0`). Commit style `refactor: <desc>` — NO attribution footer, NO Co-Authored-By.
- **ZERO behavior change**: no signature/type/namespace/bus-key/manifest-key renames; no header (`include/hades/**`) edits; no `tests/`, `app/`, `tools/`, `web/` edits (except the Task 7 docs). If a change "improves" code beyond the merge rules below, it is out of scope.
- Never stage `memory/facts.md` or `skills/` (agent-runtime churn).
- Pure moves use `git mv`. Merges: create the new file, `git rm` the olds, same commit.
- CMake edits are **text-match replacements** (find the exact existing line, replace it) — never by line number.

**MERGE PROCEDURE (applies to every "merge" step; the merge rules of spec §Merge rules):**
1. New file starts with the header comment given verbatim in the task.
2. Then the **union of all source files' `#include` lines, deduped** (system headers first, then project `"hades/..."` headers — match the existing house order).
3. Then, for each source file in the order listed: a section comment
   `// ── <short purpose> (was <old path>) ──────────────────────────────`
   followed by that file's content **verbatim minus** its leading file-header comment block and minus its `#include` lines. Keep each source's `namespace hades { ... }` / `namespace { ... }` blocks as-is — multiple such blocks in one TU are legal C++; do NOT re-flow code between namespaces.
4. **File-local collision rule:** a duplicate-symbol compile error after a merge means two file-local helpers share a name. Identical bodies → keep ONE copy (delete the later duplicate). Different bodies → rename the one with fewer uses (`<name>2`-style renames forbidden — pick a descriptive name). Known collision handled explicitly in Task 4: `static split_ws` in both `src/module/tool_runner.cpp` and `src/tool/registry.cpp` (identical → keep one).
5. After assembling: build. The compiler is the merge verifier — duplicate symbols, missing includes, ODR issues all surface as errors. Then the full suite.

---

## File Structure (target, from the spec — 22 files)

```
src/core/      blackboard.cpp eventlog.cpp executor.cpp launcher.cpp session.cpp config.cpp subprocess.cpp
src/apps/      arbiter/arbiter.cpp  llm/llm.cpp  tool_runner/tool_runner.cpp  memory/memory.cpp
               embedding_memory/{embedding_memory.cpp,providers.cpp}  skills/skills.cpp  chat/chat.cpp
               serve/serve.cpp  telegram/telegram.cpp  bridge/bridge.cpp  scope/{scope.cpp,scope_main.cpp}
src/behaviors/ standard_behaviors.cpp  capability_policy.cpp
```

---

## Task 1: core — session merge, config merge, subprocess move

**Files:**
- Create: `src/core/session.cpp` (merge: `src/core/session_id.cpp` + `src/core/session_history.cpp`), `src/core/config.cpp` (merge: `src/config/manifest.cpp` + `src/config/prompt.cpp` + `src/core/version.cpp`)
- Move: `src/tool/subprocess.cpp` → `src/core/subprocess.cpp` (`git mv`)
- Delete: the merged sources; remove emptied `src/config/`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: session.cpp.** Header comment:

```cpp
// src/core/session.cpp — session identity + persisted-conversation reads
//
// Merged (2026-07-04 src reorg): session_id (launch-timestamp ids, collision-safe
// unique_fresh_path, --resume resolution) + session_history (tolerant per-session
// jsonl reader shared by the Arbiter's load_history and GET /history).
```

Assemble per the MERGE PROCEDURE from, in order: `src/core/session_id.cpp`, `src/core/session_history.cpp`. `git rm` both sources.

- [ ] **Step 2: config.cpp.** Header comment:

```cpp
// src/core/config.cpp — manifest parse + prompt assembly + version
//
// Merged (2026-07-04 src reorg): config/manifest (MOOS-style block parser, warnings,
// fatal multi-kv detection), config/prompt (assemble_system_prompt SOUL/USER +
// read_memory_layer live core memory), core/version.
```

Assemble from, in order: `src/config/manifest.cpp`, `src/config/prompt.cpp`, `src/core/version.cpp`. `git rm` all three. `rmdir src/config` stays pending until Task 5 moves `serve_config.cpp` out — check with `ls src/config`: if only `serve_config.cpp` remains, leave the dir.

- [ ] **Step 3: subprocess move.** `git mv src/tool/subprocess.cpp src/core/subprocess.cpp`. (Header `include/hades/tool/subprocess.h` untouched — shared infra: ToolRunner, registry warm, hades-shell binary.)

- [ ] **Step 4: CMake.** Text-match replacements in `CMakeLists.txt`:

| find (exact line) | replace with |
|---|---|
| `add_library(hades_core src/core/version.cpp)` | `add_library(hades_core src/core/config.cpp)` |
| `target_sources(hades_core PRIVATE src/core/session_id.cpp)` | `target_sources(hades_core PRIVATE src/core/session.cpp)` |
| `target_sources(hades_core PRIVATE src/core/session_history.cpp)` | *(delete the line)* |
| `target_sources(hades_core PRIVATE src/config/manifest.cpp)` | *(delete the line)* |
| `target_sources(hades_core PRIVATE src/config/prompt.cpp)` | *(delete the line)* |
| `target_sources(hades_core PRIVATE src/tool/subprocess.cpp)` | `target_sources(hades_core PRIVATE src/core/subprocess.cpp)` |

- [ ] **Step 5: Build + full suite.** Expect 351/351.
- [ ] **Step 6: Commit.**

```bash
git add -A src CMakeLists.txt
git commit -m "refactor: core — merge session_id+session_history, manifest+prompt+version; move subprocess to core"
```

---

## Task 2: behaviors — standard_behaviors merge + capability_policy move

**Files:**
- Create: `src/behaviors/standard_behaviors.cpp` (merge: `src/objective/stay_on_budget.cpp` + `src/objective/avoid_destructive.cpp` + `src/objective/peer_loop_guard.cpp`)
- Move: `src/objective/capability_policy.cpp` → `src/behaviors/capability_policy.cpp` (`git mv`)
- Delete: merged sources; remove emptied `src/objective/`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: standard_behaviors.cpp.** Header comment:

```cpp
// src/behaviors/standard_behaviors.cpp — the small standing Objectives (helm behaviors)
//
// Merged (2026-07-04 src reorg): stay_on_budget (USD hard cap) + avoid_destructive
// (destructive-pattern confirm backstop) + peer_loop_guard (no onward ask_agent in a
// peer-driven turn). Objectives are competing goals of ONE agent — MOOS behaviors;
// the big scoped one (capability_policy) keeps its own file next door.
```

Assemble from, in order: `src/objective/stay_on_budget.cpp`, `src/objective/avoid_destructive.cpp`, `src/objective/peer_loop_guard.cpp`. `git rm` the three.

- [ ] **Step 2:** `git mv src/objective/capability_policy.cpp src/behaviors/capability_policy.cpp`; `src/objective/` now empty (git removes it).

- [ ] **Step 3: CMake.**

| find | replace |
|---|---|
| `target_sources(hades_core PRIVATE src/objective/stay_on_budget.cpp)` | `target_sources(hades_core PRIVATE src/behaviors/standard_behaviors.cpp)` |
| `target_sources(hades_core PRIVATE src/objective/avoid_destructive.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/objective/capability_policy.cpp)` | `target_sources(hades_core PRIVATE src/behaviors/capability_policy.cpp)` |
| `target_sources(hades_core PRIVATE src/objective/peer_loop_guard.cpp)` | *(delete)* |

- [ ] **Step 4: Build + full suite.** Expect 351/351.
- [ ] **Step 5: Commit.** `refactor: behaviors/ — merge small objectives into standard_behaviors, move capability_policy`

---

## Task 3: pure app moves — arbiter, chat, scope

**Files:**
- Move (`git mv`): `src/arbiter/arbiter.cpp` → `src/apps/arbiter/arbiter.cpp`; `src/module/chat_module.cpp` → `src/apps/chat/chat.cpp`; `src/obs/scope.cpp` → `src/apps/scope/scope.cpp`; `src/obs/scope_main.cpp` → `src/apps/scope/scope_main.cpp`
- Modify: `CMakeLists.txt`; update each moved file's first-line path comment (e.g. `// src/apps/chat/chat.cpp — …`) — comment text only, nothing else

- [ ] **Step 1:** the four `git mv`s (create dirs as needed). Fix each file's `// path — purpose` first line to the new path.
- [ ] **Step 2: CMake.**

| find | replace |
|---|---|
| `target_sources(hades_core PRIVATE src/arbiter/arbiter.cpp)` | `target_sources(hades_core PRIVATE src/apps/arbiter/arbiter.cpp)` |
| `target_sources(hades_core PRIVATE src/module/chat_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/chat/chat.cpp)` |
| `target_sources(hades_core PRIVATE src/obs/scope.cpp)` | `target_sources(hades_core PRIVATE src/apps/scope/scope.cpp)` |
| `add_executable(hades-scope src/obs/scope_main.cpp)` | `add_executable(hades-scope src/apps/scope/scope_main.cpp)` |

- [ ] **Step 3: Build + full suite.** Expect 351/351.
- [ ] **Step 4: Commit.** `refactor: apps/ — move arbiter, chat, scope (pure git mv)`

---

## Task 4: app merges I — llm, tool_runner, memory, skills

**Files:**
- Create: `src/apps/llm/llm.cpp`, `src/apps/tool_runner/tool_runner.cpp`, `src/apps/memory/memory.cpp`, `src/apps/skills/skills.cpp`
- Delete: their sources (below); emptied `src/llm/`, `src/memory/`, `src/skills/`, `src/tool/`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: llm.cpp.** Header:

```cpp
// src/apps/llm/llm.cpp — the LLM app: module + OpenAI-compatible provider + cpr transport
//
// Merged (2026-07-04 src reorg): module/llm_module (bus app, executor offload, budget
// metering) + llm/openai_compat_provider (request/response mapping) + llm/http_cpr
// (thin HTTP shell).
```

Sources in order: `src/module/llm_module.cpp`, `src/llm/openai_compat_provider.cpp`, `src/llm/http_cpr.cpp`.

- [ ] **Step 2: tool_runner.cpp.** Header:

```cpp
// src/apps/tool_runner/tool_runner.cpp — the tool-execution app: module + registry + MCP
//
// Merged (2026-07-04 src reorg): module/tool_runner (TOOL_REQUEST->TOOL_RESULT, per-tool
// timeout override) + tool/registry (Tool blocks, describe/spec warm cache) + tool/
// mcp_adapter (MCP stdio call shim). Tools themselves are transient subprocesses
// (tools/*.cpp binaries) — actions, not apps; run_subprocess lives in core/subprocess.
```

Sources in order: `src/module/tool_runner.cpp`, `src/tool/registry.cpp`, `src/tool/mcp_adapter.cpp`. **Known collision (spec §Merge rules 3):** `static std::vector<std::string> split_ws(const std::string&)` exists identically in BOTH tool_runner.cpp and registry.cpp — keep the FIRST copy, drop the second.

- [ ] **Step 3: memory.cpp.** Header:

```cpp
// src/apps/memory/memory.cpp — the archival-memory app: module + keyword rank + store
//
// Merged (2026-07-04 src reorg): module/memory_module (RETRIEVED_MEMORY per turn) +
// memory/rank (pure keyword ranking; the seam embeddings plug behind) + memory/store
// (append-only jsonl).
```

Sources in order: `src/module/memory_module.cpp`, `src/memory/rank.cpp`, `src/memory/store.cpp`.

- [ ] **Step 4: skills.cpp.** Header:

```cpp
// src/apps/skills/skills.cpp — the skills-roster app: module + library scan
//
// Merged (2026-07-04 src reorg): module/skills_module (SKILLS_ANNOUNCE at attach +
// event-driven rescan on save_skill) + skills/scan (frontmatter parse, dir scan,
// announce format; valid_skill_name stays header-inline in include/hades/skills/scan.h).
```

Sources in order: `src/module/skills_module.cpp`, `src/skills/scan.cpp`.

- [ ] **Step 5: CMake.**

| find | replace |
|---|---|
| `target_sources(hades_core PRIVATE src/module/llm_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/llm/llm.cpp)` |
| `target_sources(hades_core PRIVATE src/llm/http_cpr.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/llm/openai_compat_provider.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/module/tool_runner.cpp)` | `target_sources(hades_core PRIVATE src/apps/tool_runner/tool_runner.cpp)` |
| `target_sources(hades_core PRIVATE src/tool/registry.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/tool/mcp_adapter.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/module/memory_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/memory/memory.cpp)` |
| `target_sources(hades_core PRIVATE src/memory/rank.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/memory/store.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/module/skills_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/skills/skills.cpp)` |
| `target_sources(hades_core PRIVATE src/skills/scan.cpp)` | *(delete)* |

- [ ] **Step 6: Build + full suite.** Expect 351/351.
- [ ] **Step 7: Commit.** `refactor: apps/ — merge llm, tool_runner (split_ws dedupe), memory, skills`

---

## Task 5: app merges II — serve, telegram

**Files:**
- Create: `src/apps/serve/serve.cpp`, `src/apps/telegram/telegram.cpp`
- Delete: their sources; emptied `src/config/` (serve_config was its last file), `src/telegram/`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: serve.cpp.** Header:

```cpp
// src/apps/serve/serve.cpp — the HTTP front-end app: web UI + JSON API + Serve config
//
// Merged (2026-07-04 src reorg): module/http_server_module (POST /chat|/confirm,
// GET /health|/history, CSRF authorize, TurnGate turns) + config/serve_config
// (Serve block + --serve port resolution).
```

Sources in order: `src/module/http_server_module.cpp`, `src/config/serve_config.cpp`. `src/config/` now empty — gone.

- [ ] **Step 2: telegram.cpp.** Header:

```cpp
// src/apps/telegram/telegram.cpp — the Telegram front-end app: module + parse + cpr shell
//
// Merged (2026-07-04 src reorg): module/telegram_module (long-poll, allowlist, DM-only,
// TurnGate turns, inline-keyboard confirms) + telegram/parse (tolerant Bot-API parse/
// builders, 4096 split) + telegram/cpr_telegram_api (thin network shell, method-only logging).
```

Sources in order: `src/module/telegram_module.cpp`, `src/telegram/parse.cpp`, `src/telegram/cpr_telegram_api.cpp`.

- [ ] **Step 3: CMake.**

| find | replace |
|---|---|
| `target_sources(hades_core PRIVATE src/module/http_server_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/serve/serve.cpp)` |
| `target_sources(hades_core PRIVATE src/config/serve_config.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/module/telegram_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/telegram/telegram.cpp)` |
| `target_sources(hades_core PRIVATE src/telegram/parse.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/telegram/cpr_telegram_api.cpp)` | *(delete)* |

- [ ] **Step 4: Build + full suite.** Expect 351/351.
- [ ] **Step 5: Commit.** `refactor: apps/ — merge serve (+serve_config), telegram (+parse+cpr shell)`

---

## Task 6: app merges III — bridge, embedding_memory

**Files:**
- Create: `src/apps/bridge/bridge.cpp`, `src/apps/embedding_memory/embedding_memory.cpp`, `src/apps/embedding_memory/providers.cpp`
- Delete: their sources; emptied `src/bridge/`, `src/embedding/`, `src/module/` (bridge_module + embedding_memory_module are its last files)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: bridge.cpp.** Header:

```cpp
// src/apps/bridge/bridge.cpp — the agent↔agent bridge app: module + wire protocol + cpr shell
//
// Merged (2026-07-04 src reorg): module/bridge_module (inbound /ask peer turns w/ confirm
// auto-deny, /share PEER.<from>.<key> ingest, listener thread, executor share push) +
// bridge/protocol (versioned tolerant parse/build, peer-name gate) + bridge/cpr_bridge_http
// (thin outbound shell). valid_peer_name stays header-inline in include/hades/bridge/protocol.h.
```

Sources in order: `src/module/bridge_module.cpp`, `src/bridge/protocol.cpp`, `src/bridge/cpr_bridge_http.cpp`.

- [ ] **Step 2: embedding_memory.cpp.** Header:

```cpp
// src/apps/embedding_memory/embedding_memory.cpp — the semantic-memory app + its data layer
//
// Merged (2026-07-04 src reorg): module/embedding_memory_module (semantic rank per turn,
// incremental index, periodic reindex, fail-soft) + embedding/vector_cache (model-stamped
// flat store; the sqlite/ANN v2 seam) + embedding/indexer + embedding/session_turns +
// embedding/vec_math. Providers (HTTP + warm subprocess) live in providers.cpp next door.
```

Sources in order: `src/module/embedding_memory_module.cpp`, `src/embedding/vector_cache.cpp`, `src/embedding/indexer.cpp`, `src/embedding/session_turns.cpp`, `src/embedding/vec_math.cpp`.

- [ ] **Step 3: providers.cpp.** Header:

```cpp
// src/apps/embedding_memory/providers.cpp — embedding providers: OpenAI-compat HTTP + warm subprocess
//
// Merged (2026-07-04 src reorg): embedding/http_embedding_provider (/embeddings POST) +
// embedding/subprocess_embedding_provider (one-JSON-line warm child) + embedding/
// persistent_child (the long-lived child-process plumbing).
```

Sources in order: `src/embedding/http_embedding_provider.cpp`, `src/embedding/subprocess_embedding_provider.cpp`, `src/embedding/persistent_child.cpp`.

- [ ] **Step 4: CMake.**

| find | replace |
|---|---|
| `target_sources(hades_core PRIVATE src/module/bridge_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/bridge/bridge.cpp)` |
| `target_sources(hades_core PRIVATE src/bridge/protocol.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/bridge/cpr_bridge_http.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/module/embedding_memory_module.cpp)` | `target_sources(hades_core PRIVATE src/apps/embedding_memory/embedding_memory.cpp)` |
| `target_sources(hades_core PRIVATE src/embedding/vec_math.cpp)` | `target_sources(hades_core PRIVATE src/apps/embedding_memory/providers.cpp)` |
| `target_sources(hades_core PRIVATE src/embedding/http_embedding_provider.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/embedding/persistent_child.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/embedding/subprocess_embedding_provider.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/embedding/vector_cache.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/embedding/indexer.cpp)` | *(delete)* |
| `target_sources(hades_core PRIVATE src/embedding/session_turns.cpp)` | *(delete)* |

- [ ] **Step 5: Verify the tree matches the spec exactly:**

```bash
find src -name '*.cpp' | sort
```

Expected: exactly the 22 files of the spec's target tree; `src/module`, `src/embedding`, `src/bridge`, `src/telegram`, `src/llm`, `src/memory`, `src/skills`, `src/tool`, `src/obs`, `src/objective`, `src/config`, `src/arbiter` all gone.

- [ ] **Step 6: Build + full suite.** Expect 351/351.
- [ ] **Step 7: Commit.** `refactor: apps/ — merge bridge (+protocol+cpr), embedding_memory (app + providers)`

---

## Task 7: TSan lane + docs

**Files:**
- Modify: `CLAUDE.md` (one Gotcha line), `docs/manifest-reference.md` (src-path citations)

- [ ] **Step 1: TSan full suite.** `nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan` → expect 351/351 (the tsan tree reuses the same CMakeLists; ninja regenerates).
- [ ] **Step 2: CLAUDE.md.** Add ONE bullet at the TOP of `## Gotchas`:

```markdown
- **src/ reorganized 2026-07-04** (`refactor/src-apps`): `src/apps/<name>/` (one dir per Module —
  MOOS one-dir-per-app), `src/behaviors/` (Objectives), `src/core/` (shared infra incl. config/
  session/subprocess). 45→22 files; headers/tests/app/tools untouched; `Pieces:` paths in
  OLDER sections above are pre-reorg (find code via `src/apps/<module-name>/`).
```

- [ ] **Step 3: manifest-reference.md.** Update every `src/**` / `app/**` citation to the new layout (grep `src/` in the doc). Mapping: `src/config/manifest.cpp`→`src/core/config.cpp` · `src/config/prompt.cpp`→`src/core/config.cpp` · `src/config/serve_config.cpp`→`src/apps/serve/serve.cpp` · `src/module/llm_module.cpp`→`src/apps/llm/llm.cpp` · `src/module/tool_runner.cpp`+`src/tool/registry.cpp`→`src/apps/tool_runner/tool_runner.cpp` · `src/module/memory_module.cpp`→`src/apps/memory/memory.cpp` · `src/module/embedding_memory_module.cpp`→`src/apps/embedding_memory/embedding_memory.cpp` · `src/skills/scan.cpp`→`src/apps/skills/skills.cpp` · `src/module/telegram_module.cpp`→`src/apps/telegram/telegram.cpp` · `src/module/bridge_module.cpp`→`src/apps/bridge/bridge.cpp` · `src/objective/*`→`src/behaviors/*` (if cited) · `app/agent_wiring.cpp`, `app/hades_main.cpp` unchanged.
- [ ] **Step 4: Plain suite once more** (docs can't break it, but the branch-final gate is the pair). Expect 351/351.
- [ ] **Step 5: Commit.** `refactor: docs — reorg gotcha in CLAUDE.md, manifest-reference paths updated`

---

## Verification (end-to-end)

1. `find src -name '*.cpp' | wc -l` → 22; tree matches the spec.
2. Full plain suite 351/351 + full TSan lane 351/351.
3. `git diff main --stat -- include tests app tools web` → EMPTY (nothing outside src/CMake/docs).
4. `./build/hades manifests/dev.hades --serve` still runs (binary unchanged in behavior).

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus per `feedback_sdd_implementer_opus`), per-task review, final whole-branch review, then finishing-a-development-branch (merge ff to main — no remote, never push).
