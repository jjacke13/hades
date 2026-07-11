# core_memory — bounded, editable core memory (design)

**Date:** 2026-07-11 · **Status:** approved (Vaios) · **Branch:** `feat/core-memory`

## Problem

`pin_fact` is append-only and unbounded: the agent cannot revise or retire a fact, and the
always-in-context file (`memory/facts.md`) either stays empty (agent under-saves) or grows
stale without bound (every char is paid in EVERY turn's prompt). Observed live — the
memory-v2 "corpus quality" weakness.

Hermes-agent (Nous Research) validates the fix: a **hard char cap** on always-on memory plus
**edit actions** (add/replace/remove), where an over-cap write fails with the current entries
listed so the model consolidates in the same turn. The cap is the forcing function that makes
the agent curate.

## Decision

Replace `pin_fact` with one new native tool **`core_memory`** (clean break — repo unpublished,
no compat burden). Same file, same Arbiter fold, same allow-band. New: three actions and a cap.

## Tool: `core_memory` (native subprocess, `tools/core_memory_main.cpp`)

argv (fixed by wiring, never LLM-chosen): `[1]` = memory file (fallback `memory/facts.md`),
`[2]` = char cap (fallback / garbage / `<=0` → **2400**).

One JSON line in/out (house protocol). `describe` schema: `action` (string, required — one of
`add` | `replace` | `remove`), `text` (string), `match` (string). Per-action validation;
**empty string = absent** (the `833b9aa` exactly-one-of lesson: LLMs fill every property).

### Semantics

The file is line-oriented: **every non-empty line is an entry**. `add`/`replace` write
canonical `- <text>` bullet lines; newlines/CRs in `text` fold to spaces (pin_fact rule — one
entry, one line). All writes are **atomic** (temp + rename, house pattern). Missing file/dir
→ created on first `add`.

- **`add {text}`** — append `- <text>`.
  - Exact-duplicate line already present → `ok:false` `"already pinned: <line>"`.
  - Would exceed cap → `ok:false` with the **consolidation error** (below); nothing written.
- **`replace {match, text}`** — case-sensitive substring match over lines.
  - 0 matches → `ok:false` `"no entry matches: <match>"`.
  - \>1 matches → `ok:false` listing the matching lines (fail-closed; ambiguity never guesses).
  - exactly 1 → the whole line becomes `- <text>`.
  - Result would exceed cap → the consolidation error (no loophole via replace-with-longer).
- **`remove {match}`** — same match semantics; exactly 1 → line deleted.

### The consolidation error (the feature's core)

```
core memory full (N/2400 chars). Entries:
1. - <line>
2. - <line>
...
Consolidate: merge or drop entries with replace/remove, then retry the add.
```

Self-healing (staleness-guard style): the error carries everything the model needs to fix the
state itself, in the same turn, with no human wakeup — works identically on heartbeat/peer turns.

### Fail-closed

Malformed JSON, unknown action, non-string args, empty `text`/`match`, unwritable file →
`ok:false` with a specific error; never throws, never partial-writes.

## Config

New Session-block key **`memory_char_limit`** (default **2400** ≈ 870 tokens; Hermes uses
2200). Default lives in a header-only constant `include/hades/memory_limit.h`
(`kDefaultMemoryCharLimit`), included by wiring AND the tool (tools don't link core —
`file_version.h` precedent). Bad/absent value → default, never 0.

## Wiring (`app/agent_wiring.cpp`)

- The `pin_fact`-requires-`memory_file` MalConfig becomes the `core_memory` check (same
  reason: tool and Arbiter must target the same file or pins silently drift).
- argv append: `native + " " + core_path + " " + std::to_string(limit)` (whitespace-free
  `memory_file` already enforced by `reject_ws`).
- `memory_char_limit` parsed from the Session block next to `history_budget_chars`.

## Capability

`capability_of`: the row `save_memory || pin_fact → MemoryAppend` becomes
`save_memory || core_memory → MemoryAppend` (allow — the agent's own file, argv-fixed path,
no confirm; pin_fact precedent). `remove`/`replace` stay allow-band: same file, same blast
radius, and curation must be frictionless or it won't happen. Peer-origin turns can therefore
edit core memory via `/ask` — the existing documented bridge caveat, unchanged in scope
(v2: per-origin tool scopes).

## Retire pin_fact

Delete: `tools/pin_fact_main.cpp`, `hades-pin-fact` target + test defs (CMakeLists 85-103,
202), `tests/test_pin_fact_tool.cpp`, `tests/test_pin_fact_wiring.cpp` (both replaced by
core_memory equivalents), `package.nix` bins row. Update: `manifests/dev.hades` +
`manifests/pi.hades` Tool lines, `prompts/soul.md` (memory section + the learn-triggers line),
`docs/manifest-reference.md` (tool table, argv table, Session keys), `CLAUDE.md`.

## Unchanged

Arbiter fold (`read_memory_layer` re-reads the file every turn — edits are live immediately;
deliberately NOT Hermes' frozen-snapshot, which only pays off once we do prompt caching).
Archival `save_memory` / MemoryModule / embeddings. `memory/facts.md` stays git-tracked and
human-editable (hand-added non-bullet lines count toward the cap and are matchable — the tool
operates on lines, not a private format).

## Non-goals (v1)

Prompt-injection scan on entries (avoid_destructive arg-scan is the backstop; v2) ·
USER.md-style agent-writable profile split (static `user_file` exists) · frozen snapshot /
prompt caching · per-origin (peer/heartbeat) memory scopes.

## Testing

TDD per task. `tests/test_core_memory_tool.cpp`: describe schema; add happy/dedup/overflow
(error lists numbered entries + usage); replace 0/1/many + overflow-via-replace; remove
0/1/many; empty-string args treated absent; newline folding; garbage argv cap → default;
missing dir created; non-string args fail-closed. `tests/test_core_memory_wiring.cpp`:
MalConfig without `memory_file`; argv carries path + limit; ToolRunner round-trip writes the
configured file; `memory_char_limit` parsed (+ garbage → default); dev.hades lock test still
parses. Baseline 558/558 stays green; suite + ASan/UBSan gate every task.
