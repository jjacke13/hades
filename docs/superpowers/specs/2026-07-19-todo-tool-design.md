# todo tool + task-list fold — design

**Date:** 2026-07-19
**Status:** approved (Vaios)
**Motivation:** item 3 of the CC tool-gap analysis (CLAUDE.md): nothing tracks multi-step
work across turns — a heartbeat tick or a resumed session has no standing plan to continue.
A persisted task list, folded into every turn's context, is the smallest fix.

## Decision summary

One native tool **`todo`** doing a **whole-list replace** (the CC TodoWrite shape — models
including gpt-5.5 are heavily trained on it; one action, no match logic, every write is a
full curation), writing markdown checkboxes to **`.hades/todo.md`**, which the **Arbiter
folds into the leading system message every turn** (the `memory_file` fold pattern). New
`Capability::TodoList` → allow.

## Tool contract

`todo { items: [ { text, status? }, … ] }`:

- `items` — required array. **Empty array = clear the list** (file written empty).
- Per item: `text` required non-empty string; `status` optional ∈
  `pending | in_progress | done`, absent/empty → `pending` (empty-string-absent house
  rule). An **unknown non-empty status refuses the WHOLE call** (`ok:false`, error names
  the valid values) — no partial write; the model holds the full list and resends.
- **Caps, whole-call refusal:** more than **20 items** or any `text` over **200 bytes** →
  `ok:false` with the caps in the error text. Newlines in `text` are replaced with spaces
  (one item = one line).
- Success: `{ok:true, result:{saved:true, items:<count>}}`.
- Non-object items / non-string text / non-array items → `ok:false` (fail closed, never
  crash). Describe schema: `items` (array, required) of objects `{text: string,
  status: string}`; description tells the model this REPLACES the whole list and to keep
  it current, dropping finished work that is no longer useful context.
- Write is **atomic** (tmp + rename, parent dir created). Reply via the house
  UTF-8-replace dump.

## File format — `.hades/todo.md`

```
- [ ] a pending item
- [~] an in-progress item
- [x] a done item
```

`- [ ]` pending, `- [~]` in_progress, `- [x]` done. Human-readable/editable; lives in the
gitignored runtime dir; **survives restart AND `/new`** (it describes work, not
conversation — deliberate contrast with session history).

## Arbiter fold

Exactly the core-memory pattern: a `todo_path_` member set via **`set_todo_path(path)`**;
`start_turn()` re-reads the file each turn (tolerant: missing/unreadable/empty → no
block) and appends to the **leading `{role:system}` message** (after SOUL/USER + core
MEMORY + skills roster):

```
Your task list (keep it current with the todo tool):
- [ ] …
```

Raw file content, no reformatting. Empty path (tool not rostered) → zero cost, no read.

## Wiring / config

- Opt-in: `Tool = todo { native = ./build/hades-todo }`. No new manifest block.
- Path: **`Session.todo_file`**, default **`.hades/todo.md`** (a default is fine here —
  unlike `memory_file` there is no silent-drift risk: the tool and the fold both get the
  SAME resolved path from wiring). `reject_ws` (argv is whitespace-split).
- Wiring appends the resolved path to the tool argv (`argv[1]`) AND calls
  `arbiter->set_todo_path(path)` — single source of truth. `set_todo_path` is called
  ONLY when the `todo` tool is rostered (no tool → no fold, `todo_path_` stays empty).
- The tool binary treats `argv[1]` as the file path (fallback `.hades/todo.md` when run
  bare).

## Capability

New **`Capability::TodoList`** (enum before `Unknown`), `capability_of("todo")` →
`TodoList` → **allow**. Rationale: the agent's own plan file, path wiring-pinned (the
MemoryAppend/SessionRead class). **Heartbeat turns are the point** — a tick reads the
folded list and continues the plan unattended. Documented caveat (session_search
pattern): a `peer:`-origin turn can rewrite the plan; per-origin scopes (capability v2)
are the real fix.

## soul.md (policy only, ~3 lines)

Under a short "## Working through multi-step tasks" (or appended to the scheduling
section): when a task needs several steps or will span turns, put the plan in the `todo`
tool and update statuses as you work — the list is in your context every turn.
`schedule_task` creates FUTURE turns; `todo` is the standing plan for current work. Drop
finished items once they stop being useful context.

## Files

- `tools/todo_main.cpp` — the binary (no core link; nlohmann only).
- `src/apps/arbiter/arbiter.cpp` + its header — `todo_path_`, `set_todo_path`, fold in
  `start_turn()`.
- `include/hades/objective/capability_policy.h` + `src/behaviors/capability_policy.cpp` —
  enum + row + allow case.
- `app/agent_wiring.cpp` — `Session.todo_file` resolve + reject_ws + argv append +
  `set_todo_path`.
- `prompts/soul.md` — the policy lines.
- Tests: `tests/test_todo_tool.cpp` (replace, clear, caps, bad status, newline fold,
  atomicity via content check, describe, malformed input no-crash),
  `tests/test_arbiter.cpp` (fold present/absent/empty — appended),
  `tests/test_todo_wiring.cpp` (argv + fold end-to-end via TOOL_REQUEST then
  USER_MESSAGE → LLM_REQUEST system message contains the item; `Session.todo_file`
  override; no-tool → no fold), `tests/test_capability_policy.cpp` (+row).
- Docs: `docs/manifest-reference.md` (§4 argv table row, §5 capability row, Session-block
  `todo_file` key row), `manifests/dev.hades` commented Tool line, CLAUDE.md gap item 3.
- `package.nix` bins += `hades-todo`.

## Non-goals (recorded, not built)

Per-item ids · priorities · due dates · nested subtasks · a NOTIFY on completion ·
per-origin edit scopes (capability v2) · web-UI rendering of the list.

## Test plan

Tool-level via `run_subprocess` (the one-JSON-line protocol, no network); fold via the
existing Arbiter test style (subscribe LLM_REQUEST, post USER_MESSAGE); wiring end-to-end
with the REAL binary through ToolRunner (session_search-wiring precedent). Live smoke
(Vaios): "plan a 3-step task" → list appears in `.hades/todo.md` → next turn the agent
references it unprompted → restart → still there.
