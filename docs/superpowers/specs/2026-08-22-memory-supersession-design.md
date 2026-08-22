# Archival memory: supersession + blended ranking — design

**Date:** 2026-08-22 · **Branch:** `feat/memory-supersession` · **Status:** approved (Vaios — "do these two")

Closes the memory-v2 work-list item "dedup/decay/importance" for the ARCHIVAL layer. Prompted by
a review of [mnem](https://github.com/JustVugg/mnem) ("memory as state, not search"): its thesis —
*newer statements about a topic should supersede older ones, and ranking should blend relevance
with recency and reinforcement* — names a real hades gap. We take the **ideas**, not the
dependency (mnem is pure Python; hades ships one static musl binary with zero runtime deps, and
its `core_memory` layer already does supersession better).

## The gap

1. **Archival memory is append-only and never supersedes.** `save_memory` appends
   `{"text","ts"}`; auto-extract dedups on EXACT text only. "I prefer aisle seats" followed by
   "actually, window seats" leaves BOTH records live and BOTH recall-eligible forever. The agent
   is handed contradictory state and no way to tell which is current.
2. **Ranking is relevance-only.** `rank_memories` scores distinct query-token overlap and uses
   `ts` merely as a tie-break. A fact restated five times ranks identically to one mentioned once;
   a two-year-old fact ties with today's.

`core_memory` (add/replace/remove + cap-forced consolidation) already covers "what is true now"
for the small always-on layer. This spec brings the same discipline to the unbounded archival
layer, where it is currently absent.

## Decisions

### 1. Supersession at RANK time, keyed on an optional `topic`

- `MemoryRecord` gains `std::string topic` (default `""`).
- `save_memory` gains an OPTIONAL `topic` arg. Store lines become `{"text","ts","topic"?}`.
- Among records sharing the same **non-empty** topic, only the one with the **greatest `ts`** is
  eligible for retrieval. Older same-topic records are suppressed.
- **Empty topic = no supersession** — the exact behavior of every record written to date, so the
  change is backward compatible by construction.

**Why rank-time, not write-time rewriting:**
- The store stays **append-only** — no rewrite, no torn-file window, no concurrent-writer hazard
  (auto-extract's background worker appends to the same file).
- History is preserved on disk: the superseded value is still auditable, which is the
  "provenance/audit" backlog item, not a casualty of this one.
- It is a change to `rank_memories`, a **pure function explicitly documented as the v2 seam** —
  contained, unit-testable, no IO.

### 2. Blended score: relevance + recency + reinforcement

`rank_memories` keeps its hard admission filter (**relevance > 0**), then ranks survivors by a
weighted blend. Recency and reinforcement are *boosters among relevant records*, never admission
criteria — otherwise a recent irrelevant fact would flood the block.

| term | definition | range |
|---|---|---|
| relevance | `matched_query_tokens / total_query_tokens` | (0, 1] |
| recency | `0.5 ^ (age_days / kRecencyHalfLifeDays)` | [0, 1] |
| reinforcement | `1 - 1/(1 + bucket_size)`, `bucket_size` = records sharing this non-empty topic (untopiced → 1) | [0.5, 1) |

`score = kRelevanceWeight * relevance * (1 + kRecencyWeight*recency + kReinforcementWeight*reinforcement)`

**The boosts multiply, they do not add** (corrected during Task 1 review — the first draft of this
spec had them additive, which was wrong). Relevance is a *ratio*, `matched/|query|`, and the live
`MemoryModule` passes the entire user message as the query, so `|query|` is typically 10–40 tokens
and one matched token is worth only `1/|query|`. Against a fixed additive `+0.3` recency bonus, a
single incidental match in a fresh record would outrank four substantive matches in an old one for
any query of ≥3 distinct tokens — the exact "recall surfaces chit-chat" failure this feature is
supposed to reduce. As a multiplier the boosts are scale-invariant in `|query|` and bounded: a
record can never be overtaken by one worth less than `1/(1+0.3+0.2)` of its relevance.

Constants (`include/hades/memory/rank.h`, the tuning seam — **no new manifest keys in v1**):
`kRelevanceWeight = 1.0`, `kRecencyWeight = 0.3`, `kReinforcementWeight = 0.2`,
`kRecencyHalfLifeDays = 30.0`.

Relevance stays dominant by weight, so a strongly-matching old fact still beats a weakly-matching
new one — the blend refines the existing order rather than replacing it.

**Determinism:** sort by score desc, then `ts` desc, then `text` asc. Fully ordered, no ties left
to implementation-defined sort behavior.

**Purity preserved.** `age_days` needs a clock, which would make the ranker impure and untestable.
So: the existing 3-arg `rank_memories(all, query, top_n)` is kept (reads the system clock and
delegates), and a 4-arg `rank_memories(all, query, top_n, now)` overload takes the reference time
for deterministic tests. Every existing caller compiles unchanged.

## Backward compatibility (a hard requirement)

- Old store lines have no `topic` → parsed as `""` → never superseded → today's behavior.
- Old records keep their `ts`, so recency applies to them too. Untopiced legacy records are never
  suppressed, but their relative ORDER can shift where the blend says it should (a recent record
  can outrank an older, slightly-more-relevant one — bounded by the 1.36× ratio). Parsing and the
  written line shape are unchanged; ordering is deliberately not frozen. (An earlier draft of this
  spec claimed legacy ordering never changes, reasoning that old records are epoch-era and score
  recency ≈ 0 — wrong: a real store carries live epoch seconds.)
- **The existing `tests/test_memory_rank.cpp` and `tests/test_memory_store.cpp` assertions MUST
  pass UNCHANGED.** They encode intended behavior; needing to weaken one is a signal the design
  is wrong, not permission to edit the test.
- `MemoryRecord` stays an aggregate: `{"text", ts}` brace-init still compiles (topic defaults).

## Scope boundaries (deliberate, documented)

- **auto-extract keeps writing untopiced facts.** Giving the aux model a topic contract means
  changing its reply format from a JSON array of strings to objects — a real risk with weak
  models, and a separate change. Auto-extracted facts therefore behave exactly as today. v2 seam.
- **The embeddings path is unchanged.** `VectorCache` stores `{text, src}` with no topic or ts, so
  semantic recall cannot apply supersession; a superseded fact can still surface via
  `RETRIEVED_MEMORY_SEMANTIC` when `Module = embedding_memory` is rostered. Documented limitation;
  the fix (topic in the model-stamped, rebuildable cache) belongs with the sqlite/vector storage
  swap already on the memory-v2 list.
- **No decay-based deletion.** Nothing is ever removed from the store by this change.

## soul.md

The agent must know the feature exists or it will never pass a topic. One short paragraph: when
saving a fact that REPLACES something previously saved (a preference, a setting, a status), pass
the same short `topic` slug both times so the newer one wins; leave `topic` off for standalone
observations.

## Testing

- Pure ranker (4-arg overload, fixed `now`): supersession keeps newest-per-topic and drops older;
  untopiced records never suppress each other; distinct topics coexist; relevance still dominates
  a recency advantage; recency reorders equal-relevance records; reinforcement lifts a restated
  topic over a once-mentioned one; empty query → empty; `top_n` cap; full determinism on ties.
- Store: `topic` round-trips; a line without `topic` loads as `""`; a non-string `topic` is
  tolerated (→ `""`), never throws.
- Tool: `save_memory` writes `topic` when given, omits/empties it when absent; non-string topic
  fails closed per house rule; describe schema advertises `topic` as optional.
- Both sanitizer lanes (ASan+UBSan and TSan). Baseline **798/798**.

## Pieces

`include/hades/memory/record.h` (topic) · `include/hades/memory/rank.h` (weights + 4-arg overload)
· `src/apps/memory/memory.cpp` (ranker + tolerant topic load) · `tools/save_memory_main.cpp`
(topic arg + schema) · `tests/test_memory_{rank,store}.cpp`, `tests/test_save_memory_tool.cpp` ·
`prompts/soul.md` · `docs/manifest-reference.md` · `CLAUDE.md`.
