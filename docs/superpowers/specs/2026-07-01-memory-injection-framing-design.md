# Memory-injection framing — design spec

**Date:** 2026-07-01
**Status:** approved (brainstorm via Q&A; design confirmed "ok"). Ready for plan.

## Goal

Make the agent treat injected retrieved memory as its **own recall**, not as user-quoted text. Today the
Arbiter injects one bare `{role:system}` block `"Relevant memories:\n<merged bullets>"` before the last user
message; the block mixes saved facts with past-conversation excerpts with **no framing**. Result (observed
live, PPQ `text-embedding-3-small`): for *"where are those spaceX files you downloaded?"* the relevant past-
session turn **was** retrieved and injected, yet the agent answered *"this is our first exchange"* /
*"you're quoting back a response I gave"* — it did not recognize the injected session excerpt as its own
memory. Fix = **two labeled sub-blocks** (saved facts vs past-conversation excerpts) with framing that names
them as the agent's memory, plus a soul-prompt update, plus a staleness caveat so the agent does not assert a
stale past action as current fact.

## Current state (what exists)

- **`MemoryModule`** (keyword) posts `RETRIEVED_MEMORY` = archival saved-facts (`.hades/memory.jsonl`) ranked
  by keyword, rendered as `"- <text>\n"` bullets.
- **`EmbeddingMemoryModule`** posts `RETRIEVED_MEMORY_SEMANTIC` = cosine hits over the `VectorCache`, which
  holds **both** archival facts (`src="memory"`) **and** past-session turns (`src="session"`, text
  `"U: …\nA: …"`), rendered as one `"- <text>\n"` list — facts and session excerpts **undifferentiated**.
- **Arbiter `start_turn`** merges the two keys (`merge_memory_blocks`, dedup) and injects one
  `{role:system}` block `"Relevant memories:\n" + merged` immediately before the last user message. Absent
  both keys → no block.
- **`VectorCache::query`** returns `std::vector<ScoredMemory{std::string text; float score;}>` — **no `src`**,
  so the module cannot tell a fact hit from a session hit.
- **`prompts/soul.md`** describes only `save_memory` keyword archival and states *"Retrieval is keyword-based
  for now, not semantic"* (now false) — and **never mentions** that past-session excerpts are injected, so the
  model has no standing instruction to recognize them as recall.

## Components

### 1. `VectorCache::query` carries `src`
- Add `std::string src;` to `ScoredMemory` (the `Rec` in `mem_` already stores `src`; `query` just copies it
  into each result). No behavior change to ordering/floor/top_n. Existing `test_vector_cache` assertions on
  `text`/`score` stay valid; add one asserting `src` round-trips through `query`.

### 2. `EmbeddingMemoryModule` posts two keys (partition by src)
- After the cosine `query`, **partition** the top-N hits into fact hits (`src=="memory"`) and session hits
  (`src=="session"`), render each as `"- <text>\n"` bullets, and post:
  - `RETRIEVED_MEMORY_SEMANTIC` = the **fact** hits (key unchanged; now facts-only).
  - `RETRIEVED_SESSION_SEMANTIC` = the **session-excerpt** hits (**new** key).
- Both are `""` when empty (same fail-soft: any error / no hits → both `""`). The per-turn `top_n`/`min_similarity`
  still apply to the combined query; partition is just a post-split of the surviving hits (so a turn with
  `top_n=3` yields at most 3 hits total, split across the two keys).

### 3. Arbiter — two labeled sub-blocks with framing
- In `start_turn`, read `RETRIEVED_MEMORY` (kw facts) + `RETRIEVED_MEMORY_SEMANTIC` (semantic facts) +
  `RETRIEVED_SESSION_SEMANTIC` (session excerpts). Build up to two labeled sub-blocks:
  - **Facts** = `merge_memory_blocks(RETRIEVED_MEMORY, RETRIEVED_MEMORY_SEMANTIC)` (dedup, keyword-first).
    Labeled: `"Facts from your memory (you saved these earlier; treat as reliable):"`.
  - **Conversations** = `RETRIEVED_SESSION_SEMANTIC` (session excerpts). Labeled:
    `"Excerpts from earlier sessions with this same user — your own memory of past conversations. Treat them "`
    `"as things you and the user already discussed (do NOT say this is a first exchange, and do NOT treat "`
    `"them as the user quoting you). They record what was SAID then and may be out of date — re-verify current "`
    `"state (files, live data, tool results) before asserting a past action's result still holds."`
  - Inject one `{role:system}` message: the non-empty sub-blocks joined (Facts first, then Conversations),
    each under its label, placed immediately before the last user message (unchanged position). If **both**
    are empty → **no** block injected (backward-compatible with "no memory this turn").
- The old single `"Relevant memories:\n…"` string is **replaced** by this labeled structure. This intentionally
  changes the injected wording (see Testing).
- Keep the injection ephemeral (recomputed each turn, never stored in `history_`).

### 4. `prompts/soul.md`
- **Fix the stale line:** replace *"each turn the most relevant entries are recalled by keyword match … (Retrieval
  is keyword-based for now, not semantic.)"* with the accurate description: archival is recalled each turn by
  keyword **and** (when the `embedding_memory` app is enabled) by **semantic similarity**; past **sessions** are
  also semantically recalled.
- **Add a standing paragraph** on how to treat the injected block: *the memory block injected before the user's
  message is YOUR memory — saved facts plus excerpts of earlier sessions with this same user. Recognize it as
  recall: reference past conversations naturally, never claim a "first exchange" when memory is present, and
  never treat the excerpts as the user quoting you. Session excerpts record what was said before and may be
  stale — re-verify current state (files, live data) before asserting a past action's result still holds.*
- Keep it concise (a few sentences); soul.md is prepended every turn, so cost matters.

## Data flow

```
USER_MESSAGE
  MemoryModule (keyword)       -> RETRIEVED_MEMORY            (facts)
  EmbeddingMemoryModule        -> RETRIEVED_MEMORY_SEMANTIC   (semantic facts,   src=memory)
                               -> RETRIEVED_SESSION_SEMANTIC  (session excerpts, src=session)  [NEW]
  Arbiter.start_turn:
    facts  = merge_dedup(RETRIEVED_MEMORY, RETRIEVED_MEMORY_SEMANTIC)
    convos = RETRIEVED_SESSION_SEMANTIC
    inject {role:system}: [ "Facts from your memory …:\n"+facts ] + [ "Excerpts from earlier sessions …:\n"+convos ]
            (whichever non-empty; both empty -> no block) before the last user message
```

## Error handling / backward-compat

- Any embedder failure → both semantic keys `""` (fail-soft, unchanged) → Facts falls back to keyword-only,
  Conversations empty. Never crashes a turn (existing try/catch retained).
- The embedding module inert unless rostered (`Module = embedding_memory`) → `RETRIEVED_SESSION_SEMANTIC` never
  posted → Arbiter sees only `RETRIEVED_MEMORY` → Facts block only. So a keyword-only agent still gets a
  single labeled Facts block (its wording changes from `"Relevant memories:"` to `"Facts from your memory…"`,
  which is fine and more accurate).
- The `RETRIEVED_MEMORY_SEMANTIC` key **meaning narrows** (was facts+sessions, now facts-only). The Arbiter is
  its only consumer; the new `RETRIEVED_SESSION_SEMANTIC` carries the sessions. No external consumer breaks.

## Testing (TDD)

- **`VectorCache`:** `query` results carry the record's `src`.
- **`EmbeddingMemoryModule`:** a query hitting a `src="memory"` record posts it on `RETRIEVED_MEMORY_SEMANTIC`
  and NOT on `RETRIEVED_SESSION_SEMANTIC`; a `src="session"` hit posts on `RETRIEVED_SESSION_SEMANTIC` and NOT
  the fact key; a mix splits correctly; no hits → both `""`; provider failure → both `""` (fail-soft).
- **Arbiter:** given the three keys, the injected `{role:system}` block contains the **Facts** label + facts
  and the **Conversations** label + session excerpts, in that order, before the last user message; only the
  Facts block when sessions absent; only Conversations when facts absent; **no block** when all three empty
  (backward-compat). **Existing memory-injection tests that assert the exact `"Relevant memories:\n…"` string
  (e.g. `SemanticAbsentIsUnchanged`, `MergesKeywordAndSemanticMemoryDeduped`) are updated** to the new labels
  — this is an intentional wording change, not a regression.
- **soul.md:** covered by the existing prompt-assembly tests only insofar as they don't hard-assert the removed
  sentence; if a test pins soul.md content, update it. (No new soul.md unit test — it's persona text.)
- **Live smoke (Vaios):** restart with `embedding_memory` enabled; ask *"where are those spaceX files?"* (or any
  past-session topic) → the agent references the past conversation as its own memory (no "first exchange"), and
  does NOT assert stale files exist without re-checking.

## Out of scope (noted)

- Truncating/summarizing long session-turn units (a full assistant answer as one unit is coarse) — a separate
  v2 quality item already logged.
- Changing retrieval ranking / the floor / top_n (config-tunable already).
- The sqlite/binary-vector v2 store switch (separate, already logged).

## MOOS-IvP framing

The retrieved-memory block is a **sensor reading fused into the helm's context** — this change labels the
provenance of that reading (curated facts vs logged past missions) and tells the helm how much to trust each:
saved facts are standing truth; past-session excerpts are the mission log (what happened then, re-verify the
world before acting on it).
