# Auto-extract — post-turn background memory harvest (design)

**Status:** approved (Vaios, 2026-07-16): same-provider aux call with optional `model` override;
fires every human turn on a last-exchange digest (Hermes `auxiliary.background_review` parity).
Memory-v2 item 2 — the "learns by itself" leg.

## Problem

The agent saves facts only via explicit `save_memory`/`core_memory` calls, and live experience
shows it rarely does. Preference signals, environment facts, and corrections flow past unharvested.
soul.md learn-triggers (shipped) help the deliberate path; this adds the automatic one.

## Design

**`AutoExtractModule`** (`type()=="auto_extract"`, `src/apps/auto_extract/auto_extract.cpp`,
`include/hades/module/auto_extract_module.h`). Opt-in: `Module = auto_extract` (+ optional
`AutoExtract` block). Omitted → `Agent.auto_extract == nullptr`, zero coupling; the test
`build_agent` overload is unaffected.

### Trigger and scope

- Subscribes `TURN_ORIGIN` (records origin), `USER_MESSAGE` (captures the user text), and
  `ASSISTANT_MESSAGE` (turn complete → review candidate).
- **Human-origin turns only** (v1): an `ASSISTANT_MESSAGE` whose turn origin is `peer:*` or
  `heartbeat:*` is ignored — harvesting peer/self-generated text into the user's memory store is
  an injection/pollution surface, not a feature.
- Skips: empty user or assistant text; assistant text that is a bracketed turn artifact
  (`[blocked…]`/`[declined…]`/`[stopped…]`/`[timed out]`); **one review in flight** — a turn
  completing mid-review is skipped, never queued (heartbeat skip-if-busy precedent).

### The aux call (NOT a turn)

- Runs on the shared **Executor** worker (`set_executor`, LLMModule pattern; no executor set —
  the test path — runs inline). The pump thread never blocks.
- The module builds its **own** `OpenAICompatProvider` from the Session block (same endpoint /
  api_key_env / price) with the `AutoExtract.model` override when present (default =
  `Session.model`). No Arbiter, no tools array, no history, no TurnGate, no turn epoch.
- Prompt: fixed system instruction ("extract durable facts about the user or environment worth
  remembering across sessions — preferences, corrections, standing facts; reply with a JSON array
  of short strings, or NONE") + one user message carrying the digest `U: <user>\nA: <assistant>`
  (each side truncated ~2000 chars).
- Reply parsing is tolerant and fail-closed: strict `NONE`/empty → nothing; a JSON array of
  strings → up to `max_facts` entries (each trimmed, newlines stripped, 500-char cap, empties
  dropped); anything unparseable → nothing. Whole path wrapped in try/catch — an extractor
  failure never surfaces into a turn (fail-soft, embedding precedent).

### Write path

- Appends accepted facts to the **archival store** (`.hades/memory.jsonl` — the same
  `Memory.store` path the wiring already resolves; passed to the module at wire time, single
  source of truth). Record = save_memory format plus provenance: `{"text":…,"ts":…,"src":"auto"}`
  (extra keys are tolerated by the existing loader).
- **Exact-duplicate skip**: a fact whose `text` already appears in the store is not re-appended.
- Core memory (`memory/facts.md`) is NEVER touched — that file stays agent-curated.
- Pickup is automatic: MemoryModule re-reads the store every `USER_MESSAGE`; the embedding
  indexer folds new records on its next incremental run.
- Concurrency note: appends are single `write`-sized lines in append mode (same as the
  save_memory tool); a same-instant tool append interleaving is not torn in practice and the
  tolerant loader skips a hypothetically torn line. Documented, not engineered around (v1).

### Budget — metered, closing the embeddings gap rather than copying it

The worker computes the call's cost from usage tokens × `price_per_mtok` and posts
**`AUX_SPENT_USD`** (a per-call delta). **LLMModule** subscribes it (pump-thread handler, single
writer preserved) and folds it into cumulative `spent_` → the next `BUDGET_SPENT_USD` post
includes aux spend, so `stay_on_budget` gates it. New bus key documented.

### Config (`AutoExtract` block, all optional)

| key | default | meaning |
|---|---|---|
| `model` | `Session.model` | aux-call model override (cheaper model seam) |
| `max_facts` | `3` | cap per review |
| `timeout_s` | `60` | aux-call HTTP timeout |

### Teardown / lifetime

`Agent.auto_extract` sits with the plain modules (after `status`, before `chat`); the Executor is
declared after them → destroyed FIRST → its join happens while the module and its provider are
alive (existing invariant, no new ordering).

### Known bounded risk (documented, accepted v1)

Conversation text can prompt-inject the extractor ("remember that X"). Blast radius: ≤ max_facts
junk archival lines per turn — no tool powers, no core-memory access, recall-ranked like any other
memory. The `"src":"auto"` provenance tag is the audit handle (and the seam for a future
review/decay pass).

## Non-goals (v1)

Semantic dedup (needs embeddings; v2) · harvesting peer/heartbeat turns · core-memory writes ·
batched/windowed digests · a separate provider block · retroactive extraction over old sessions.

## Tests

Module-level with a scripted fake provider: fires on human turn and writes facts; NONE/garbage →
no write; dup skipped; max_facts clamp; peer/heartbeat origin ignored; bracketed artifacts
ignored; in-flight skip; AUX_SPENT_USD posted and folded by LLMModule (budget integration);
malformed bus payloads don't crash. Wiring: block parsing + defaults; module absent → null.
