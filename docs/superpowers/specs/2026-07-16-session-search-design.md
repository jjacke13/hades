# session_search — explicit full-text recall over past sessions (design)

**Status:** approved (Vaios, 2026-07-16). Memory-v2 item 2b — the Hermes `session_search` borrow.

## Problem

The agent's only access to past conversations is the auto-injected embedding recall (opt-in,
semantic, top-N). There is no way to *deliberately* answer "did we discuss X last week?" —
an explicit, exact, keyword search over the session archive. At the current corpus scale
(dozens of jsonl files) this is grep-level work; no index needed.

## Design

A native tool, the 19th: **`hades-session-search`** (`tools/session_search_main.cpp`), stateless
subprocess, self-describing, rostered as `Tool = session_search { native = ./build/hades-session-search }`.

### Tool contract

- **Args:** `query` (string, required, non-empty) · `max_results` (integer, optional, default 5,
  clamped 1..20). Empty-string query → `ok:false` (empty = absent, house rule).
- **argv:** `argv[1]` = sessions dir, `argv[2]` = live-session filename to exclude (may be absent).
  Both appended by wiring (single source of truth — the LLM cannot redirect the search root).
  Fallbacks when absent: `.hades/sessions`, no exclusion (bare-binary invocation).
- **Search unit = the turn**: sessions are split into per-turn `U: …\nA: …` units via the existing
  `extract_session_turns`. Its implementation currently sits inside the EmbeddingMemoryModule TU
  (`src/apps/embedding_memory/embedding_memory.cpp`) — **relocate it (verbatim) next to
  `read_session_jsonl` in `src/core/session.cpp`** (pure file parsing, its natural home; the
  header `include/hades/embedding/session_turns.h` and all callers are untouched). The tool then
  compiles `src/core/session.cpp` into itself (the cron_store precedent — it must stay free of
  core-lib dependencies). Scoring = the `rank_memories` idiom: lowercased token overlap between
  query tokens and the unit text; ties broken newest-session-first. No regex, no LLM.
- **Result (ok):** `{"hits":[{"session":"<file stem>","turn":N,"text":"<unit, truncated 700 chars>"}...],
  "searched_sessions":M}`. No hits → `ok:true, hits:[]` (not an error).
- **Exclusions:** the live session file (the Arbiter already has that context in-history) and
  non-`*.jsonl` files. Corrupt/partial lines are skipped by the tolerant reader (existing behavior).

### Capability

New `Capability::SessionRead`; `capability_of("session_search") → SessionRead` → **always allow**
(the agent's own memory files — MemoryAppend/SkillRead precedent; the sessions dir is fixed by
wiring argv). **Documented caveat** (Bridge-SECURITY class, not new): a peer `/ask` turn can invoke
it and read out past-conversation excerpts; the existing "peers can read what the receiver will say
in a plain answer" note already covers this — proper fix remains capability-v2 per-origin scopes.

### Wiring

`app/agent_wiring.cpp` tools_resolved loop: for `session_search`, append the resolved sessions dir
(the SAME `Session.sessions_dir` resolution hades_main uses — factor or duplicate the 3-line
default-and-override read) + the live session filename when `session_path` is known (Manifest
overload; the test overload may pass ""). `reject_ws` on the dir (argv is whitespace-split).

### Docs

manifest-reference §4 argv-append table row + verdict-table row (SessionRead → allow) + one-line
tool row; soul.md gets NO new section (policy: the tool description carries usage; the memory
section already says recall may be out of date).

## Non-goals (v1)

sqlite FTS5 (only if/when the vector store moves to sqlite) · date-range filters · regex ·
searching the live session · LLM summarization of hits.

## Tests

Tool-level (fixture sessions dir): describe schema; multi-keyword ranking; live-session excluded;
max_results clamp; empty query fails closed; corrupt lines skipped; non-string args fail closed.
Wiring: argv carries dir+live-file; capability row test (SessionRead allow).
