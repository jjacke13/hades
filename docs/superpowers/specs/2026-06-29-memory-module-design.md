# hades MemoryModule — design spec

**Date:** 2026-06-29
**Status:** approved (brainstorm) → ready for plan

## Goal

Give the hades agent **dynamic memory**. Today the MEMORY layer is a STATIC file folded into the
system prompt once at startup. Replace/augment it with a MOOS-app-style module that **persists**
facts the agent chooses to remember and **retrieves** the relevant slice per turn (mem0/MemGPT-style
RAG, v1 = keyword).

MOOS-IvP analogy: a dedicated state-keeping app on the Blackboard (like a contact-manager/logger that
maintains state) — NOT an Objective, NOT inside the Arbiter.

## Core split: tool writes, app reads

Two halves share one file store:

- **save = a native tool** (`save_memory`), like `write_file`/`fs_read`. The agent (LLM) decides what
  to remember by calling it. Append-only.
- **retrieval = the custom app** (`MemoryModule`) on the Blackboard. Each turn it loads the store,
  ranks against the user message, and posts the relevant slice for the Arbiter to inject.

The tool is a separate subprocess, so the store MUST be on disk (a file) — which is also the natural
v1 store and matches the "libraries/file-first, replace later" project pattern.

## v1 locked choices

| fork | v1 | v2+ |
|---|---|---|
| write trigger | explicit — agent calls `save_memory` tool | auto-extract per turn (LLM) |
| store | flat file, JSONL, append-only | sqlite / vector |
| retrieval algo | keyword top-N | embeddings (cosine), same interface |
| retrieval location | `MemoryModule` app on Blackboard | unchanged |
| delivery | separate ephemeral `{role:system}` block before the user msg | unchanged |

## Components

### 1. `save_memory` native tool — `tools/save_memory_main.cpp` → binary `hades-save-memory`

Self-describing, one-JSON-line protocol like the other 5 tools.

- `{"call":"describe"}` →
  `{"ok":true,"result":{"name":"save_memory","description":"Persist a fact/observation to long-term memory.","schema":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}}`
- `{"call":"save_memory","args":{"text":"..."}}` → append one line to the store file (path from
  `argv[1]`, fallback default `.hades/memory.jsonl`), then
  `{"ok":true,"result":{"saved":true}}`. Empty/missing `text` → `{"ok":false,"result":{"error":"..."}}`.

Record line format (one JSON object per line):
```json
{"text":"<string>","ts":<unix_seconds_double>}
```

**Security/safety:** append-only to the agent's own dedicated store → **NOT** added to the
`avoid_destructive` mutating-tools veto set. No confirm gate (unlike `write_file`, which can
overwrite arbitrary paths). The store path is fixed by config, never chosen by the LLM.

### 2. `MemoryStore` — `src/memory/store.{h,cpp}`

Pure-ish file IO, no Blackboard dependency.

```cpp
struct MemoryRecord { std::string text; double ts; };
std::vector<MemoryRecord> load_memories(const std::string& path);   // missing file -> {}; bad line -> skip
```

- Missing file → empty vector (not an error — fresh agent).
- Malformed JSON line → skip and continue (never throw).

### 3. Keyword ranker — `src/memory/rank.{h,cpp}` (pure function)

```cpp
std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n);
```

- Tokenize lowercase on non-alphanumeric runs (query + each record text).
- score = number of distinct query tokens present in the record's token set.
- Drop records with score 0.
- Sort by score desc, then `ts` desc (recency tie-break).
- Return at most `top_n`.

Pure → unit-testable without files or network. **This is the seam** v2 embeddings drop into (same
signature, different scorer).

### 4. `MemoryModule` — `src/module/memory_module.{h,cpp}`, `type() == "memory"`

```cpp
class MemoryModule : public Module {
  std::string type() const override { return "memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;   // reads store path + top_n
  void on_attach(Blackboard& bb) override;                    // subscribes USER_MESSAGE
private:
  std::string store_path_; std::size_t top_n_ = 5; Blackboard* bb_ = nullptr;
};
```

- `on_start`: `store_path_` from cfg (`store`, default `.hades/memory.jsonl`); `top_n_` from cfg
  (`top_n`, default 5).
- `on_attach`: subscribe `USER_MESSAGE` → `load_memories(store_path_)` → `rank_memories(.., msg, top_n_)`
  → render a block string → `post("RETRIEVED_MEMORY", <string>, "memory")`. Empty result → post `""`.

Render format (one memory per line):
```
- <text>
- <text>
```

### 5. Arbiter delivery change — `src/arbiter/arbiter.cpp`

`start_turn()` currently builds `messages = [system_prompt] + history_`. Change: after building, read
latest `RETRIEVED_MEMORY`; if non-empty, splice an ephemeral message
`{"role":"system","content":"Relevant memories:\n"+mem}` **immediately before the last user message**
in the array. This message is NOT stored in `history_` (ephemeral, recomputed each turn — never
accumulates stale memory).

On tool-continuation turns (no new user message) the latest `RETRIEVED_MEMORY` from the last user turn
may still be injected; harmless (same context), keeps v1 simple.

## Wiring — `app/agent_wiring.cpp`

- Read a new `Memory` block (`store`, `top_n`) from the manifest. Build `MemoryModule` with it.
- **Register `MemoryModule` BEFORE the Arbiter.** Rationale: single-threaded pump dispatches to
  subscribers in registration order; on a `USER_MESSAGE`, MemoryModule's handler runs first and its
  `post("RETRIEVED_MEMORY")` updates the latest-value map synchronously, so the Arbiter's
  `start_turn()` (same `USER_MESSAGE` dispatch) sees the fresh slice.
- **One source of truth for the store path:** wiring takes the path from the `Memory` block, passes it
  to `MemoryModule`, AND appends it as the `save_memory` tool's argv (overriding the literal in the
  Tool command) so tool and app can never drift.

## Manifest additions — `manifests/dev.hades`

```
Memory { store = .hades/memory.jsonl  top_n = 5 }
Tool = save_memory { native = ./build/hades-save-memory .hades/memory.jsonl }
```

## New Blackboard key

| key | payload | producer → consumer |
|---|---|---|
| `RETRIEVED_MEMORY` | string (rendered block; `""` if none) | MemoryModule → Arbiter |

## Data flow

```
USER_MESSAGE ──► MemoryModule: load store, rank vs msg, post RETRIEVED_MEMORY
             └─► Arbiter.start_turn(): inject ephemeral memory system-block before user msg ──► LLM

LLM tool_call save_memory ──► ToolRunner subprocess (hades-save-memory) ──► append line to store
                                                                          (visible to next turn's retrieval)
```

## Error handling

- Missing store file → empty memory list (fresh agent), `RETRIEVED_MEMORY=""`.
- Malformed JSONL line → skipped, rest still loaded.
- `save_memory` with empty/missing `text` → `{ok:false}`, no write.
- Empty query or zero keyword overlap → `RETRIEVED_MEMORY=""` → Arbiter injects nothing.

## Testing (TDD, GoogleTest)

- `test_memory_rank` — pure ranker: keyword overlap scoring, recency tie-break, `top_n` cap, zero-overlap → empty.
- `test_memory_store` — load/parse, skip malformed line, missing file → empty.
- `test_save_memory_tool` — `describe` yields spec; a call appends a well-formed line (subprocess, via `SAVE_MEMORY_BIN` compile def like `FS_READ_BIN`).
- `test_memory_module` — `USER_MESSAGE` with a seeded store → `RETRIEVED_MEMORY` contains expected memory, excludes non-matching.
- `test_arbiter` (extend) — with `RETRIEVED_MEMORY` set, `LLM_REQUEST.messages` contains the ephemeral memory system-block immediately before the user message; with it empty/absent, no such block; block never enters `history_`.

## Out of scope (v2+)

embeddings/vector retrieval · auto-extract (LLM-summarized memories per turn) · dedup / decay /
importance scoring · tags / categories · sqlite backend · edit/delete/forget · memory via MCP ·
confirm-gating `save_memory`.

## MOOS-IvP mapping

`MemoryModule` = a MOOS app that maintains persistent state on the MOOSDB (a logger/contact-manager
analog). `save_memory` tool = an actuator the helm (Arbiter, via the LLM) can drive. `RETRIEVED_MEMORY`
= a published variable other apps (the Arbiter) consume. Consistent with: 1 agent = 1 community
(Blackboard + Arbiter + modules).
