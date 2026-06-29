# pin_fact + live core-memory layer — design spec

**Date:** 2026-06-29
**Status:** approved (brainstorm via Q&A) → ready for plan

## Goal

Give the agent a **second** memory write path: the always-on **core memory** layer — the static
`memory_file` in the SOUL/USER/MEMORY system-prompt trio — and make it **live** (re-read every turn so
the agent sees its own edits the same session). This complements the existing **searchable archival**
store (`save_memory` → `.hades/memory.jsonl`, keyword-retrieved per turn) shipped earlier today.

Mental model = MemGPT/Letta two-tier memory, but with hades-native tool names:

| layer | file | how it reaches the LLM | written by |
|---|---|---|---|
| **core** (always-on) | `memory/facts.md` | whole file, re-read **every turn**, folded into the system message | **`pin_fact`** tool |
| **archival** (searchable) | `.hades/memory.jsonl` | keyword top-N, ephemeral `{role:system}` block before the user msg | `save_memory` tool (unchanged) |

**Naming (locked):** `pin_fact` (core) + `save_memory` (archival — name unchanged, description clarified).

## Components

### 1. `pin_fact` native tool — `tools/pin_fact_main.cpp` → binary `hades-pin-fact`

Self-describing, one-JSON-line protocol like the other tools.

- `{"call":"describe"}` →
  `{"ok":true,"result":{"name":"pin_fact","description":"Pin a standing fact to your always-on core memory — kept in your context every turn. Use for identity/preferences/standing facts you always need; use save_memory instead for details to recall by keyword later.","schema":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}}`
- `{"call":"pin_fact","args":{"text":"..."}}` → append the line `- <text>\n` to the core-memory file
  (path from `argv[1]`, default `memory/facts.md`), **creating the parent directory if missing**; return
  `{"ok":true,"result":{"pinned":true}}`.
- Absent/non-string `text` → `{"ok":false,"result":{"error":"missing arg: text"}}`. Unknown call → `{"ok":false}`.

**Type-guarded** (learn from the save_memory bug): read `call` and `args.text` only when they are strings
(`is_string()` checks), never `json::value()` on a possibly-wrong type — a malformed/adversarial request
must return `ok:false`, never throw/abort. Append-only (`std::ios::app`), never truncate.

**Security/safety:** append-only to the agent's own curated file → **NOT** added to `avoid_destructive`'s
mutating-tools set; no confirm gate (same call as `save_memory`). Parent dir auto-created so a first pin
works regardless of cwd.

### 2. Prompt assembly split — `src/config/prompt.{h,cpp}`

`assemble_system_prompt(session)` currently reads three keys (`system_prompt_file`, `user_file`,
`memory_file`) once at startup. Change:

- `assemble_system_prompt` now reads **only** `system_prompt_file` (SOUL) + `user_file` (USER) — the
  static base, still `read_or_throw` (fail visibly if a configured persona file is unreadable). Drop
  `memory_file` from its key list.
- Add `std::string read_memory_layer(const std::string& path)` — tolerant whole-file read: empty path →
  `""`; missing file → `""`; never throws (the core file may not exist until the first `pin_fact`).

### 3. Arbiter live reload — `src/arbiter/arbiter.{h,cpp}`

- Add `std::string memory_path_;` + `void set_memory_path(std::string p)`.
- In `start_turn()`, build the system message fresh each turn:
  `sys = system_prompt_`; if `memory_path_` non-empty, `core = read_memory_layer(memory_path_)`; if
  `core` non-empty, `sys += "\n\n" + core`. Push `{role:system, content:sys}` only when `sys` non-empty.
- This re-reads the core file **every turn** → the agent's `pin_fact` edits are live same-session. The
  static `system_prompt_` (SOUL+USER) is still set once at startup and never re-read.

This is independent of the existing ephemeral `RETRIEVED_MEMORY` archival block (which stays before the
last user message). Core memory lives in the leading system message; archival memory in the pre-user
block. Two distinct channels, by design.

### 4. Wiring — `app/agent_wiring.cpp`

- `set_system_prompt(assemble_system_prompt(session))` (now SOUL+USER only — unchanged call site).
- `a.arbiter->set_memory_path(session.kv["memory_file"])` (empty if unset → no core layer).
- **Single source of truth for both tool paths:** the existing loop that appends the store path to the
  `save_memory` tool now also appends the **core-memory path** (Session `memory_file`) to the `pin_fact`
  tool's `native` command. Generalize the copy-then-modify so each tool gets its configured path.
- **Whitespace guard** applies to both paths: a `store` (archival) or `memory_file` (core) path
  containing whitespace → `throw MalConfig(...)` (the argv is whitespace-split downstream).

### 5. Manifest + seed — `manifests/dev.hades`, `memory/facts.md`

- Enable the core layer: `memory_file = memory/facts.md` in the Session block (uncomment).
- Add `Tool = pin_fact { native = ./build/hades-pin-fact }` (bare binary; wiring appends the path).
- Seed `memory/facts.md` with a one-line header comment so the dir exists and the file has a home.

### 6. Persona self-knowledge — `prompts/soul.md`

Update the "How you work" memory section to describe **both** layers/tools precisely: `pin_fact` =
always-on core memory (`memory/facts.md`, in context every turn), `save_memory` = searchable archival
(`.hades/memory.jsonl`, recalled by keyword). So the agent answers accurately and picks the right tool.

## Data flow

```
pin_fact(text) ──► hades-pin-fact subprocess ──► append "- text" to memory/facts.md (creates dir)
                                                       │
Arbiter.start_turn() (every turn) ──► read_memory_layer(memory/facts.md) ──► fold into system message ──► LLM
```

## Error handling

- `memory_file` unset / file missing → `read_memory_layer` returns `""` → no core block. Not an error.
- `pin_fact` with absent/non-string `text` → `{ok:false}`, no write, no throw.
- Configured `system_prompt_file`/`user_file` unreadable → `MalConfig` (unchanged, fail visible).
- `store`/`memory_file` path with whitespace → `MalConfig` at wiring.

## Testing (TDD, GoogleTest)

- `test_pin_fact_tool` — describe yields spec; call appends `- <text>`; append-not-truncate (two calls →
  two lines); non-string text → `ok:false`, no crash; creates a missing parent dir.
- `test_prompt` (extend/adjust) — `assemble_system_prompt` now joins only SOUL+USER (memory_file no longer
  pulled in); `read_memory_layer`: unset → `""`, missing file → `""`, present file → its contents.
- `test_arbiter` (extend) — with `set_memory_path` to a seeded file, the `LLM_REQUEST` system message
  contains the core text; editing the file between turns changes the next turn's system message (live
  reload); unset path + empty system prompt → no system message.
- `test_pin_fact_wiring` — `build_agent` with a `memory_file` + `pin_fact` tool: invoking the tool writes
  `facts.md`, and the next turn's system prompt contains the pinned line (end-to-end through the graph).

## Out of scope (v2+)

`core_memory_replace`/delete/edit (append-only v1) · size cap / eviction of core memory · auto-promotion
archival→core · embeddings (archival v2, separate).
