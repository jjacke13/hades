# Session compaction (compact-and-continue) — design

**Date:** 2026-07-20 · **Branch:** `feat/compactor` · **Status:** approved (Vaios)

Closes the backlog item "Context-full behavior — DECIDE" (CC-gap item 8). Today
`windowed_history_()` silently sends only the most-recent tool-pairing-safe suffix within
`history_budget_chars`; dropped turns vanish from context with no summary, no notice, and no
recovery beyond archival/embeddings recall. Decision: **compact-and-continue** — turns that
fall out of the window are folded into a rolling per-session summary by a **background aux
LLM call**, delivered via the system-message fold, persisted in a **sidecar file**. The
`/compact` analogue, automatic, fail-soft, opt-in.

## Decisions (Vaios, 2026-07-20)

- **Shape:** compact-and-continue (not auto-rotate, not warn-only). Session identity —
  file, resume, embeddings — is preserved.
- **Who/when:** background aux call, the auto-extract pattern (zero turn latency; the
  summary may lag one turn behind a drop — that gap IS today's behavior and self-heals).
- **Storage:** sidecar `.hades/sessions/<id>.summary.md` next to the session jsonl (same-id
  pairing → `--resume` picks it up free; jsonl format untouched — `hades-scope`,
  `GET /history`, tolerant parse all unaffected).

## Architecture — `Module = compactor` (opt-in)

MOOS-shaped separation, mirroring `auto_extract`: the Arbiter detects, the module
summarizes, the bus carries. No module in the roster → nothing posts → byte-identical to
today (the fail-soft floor).

### 1. Detect — Arbiter, pump thread

- `windowed_history_()` is refactored to expose the window-start index it already computes
  (private helper returning `{start, view}`; the existing call sites keep their behavior —
  pure refactor, locked by existing tests).
- New member `summarized_upto_` = number of leading `history_` messages covered by the
  current summary (0 at boot/`/new`).
- In `start_turn()`, after computing the window: if `window_start > summarized_upto_`, post
  **`COMPACT_REQUEST`** and continue the turn normally — detection never blocks, never
  re-fires for the same span (`pending_compact_` flag until the reply lands or is
  discarded; skip-if-busy, never queued).

`COMPACT_REQUEST` payload:

```json
{
  "session": "<session id — the session_path_ filename stem>",
  "upto": <window_start>,
  "span": [ {"role": "...", "content": "<digest>"} ... ],   // history_[summarized_upto_ .. window_start)
  "current_summary": "<existing summary text or ''>"
}
```

Span digesting (Arbiter side, where history lives): per-message `content` byte-truncated
(~1000B, `trunc_utf8`-style walk-back) — tool results especially; whole span payload capped
(~24000B, oldest messages dropped first with a `[...earlier turns omitted...]` marker —
they are the least-recent information and archival recall still covers them). Digests use
the UTF-8-replace dump (house pattern).

### 2. Summarize — CompactorModule, Executor worker

`src/apps/compactor/compactor.cpp`, `type()=="compactor"`. Subscribes `COMPACT_REQUEST`
(pump thread), gates (non-empty span; one in flight via `std::atomic<bool> busy_` —
auto-extract discipline), then runs on the Executor (inline without one — test seam):

- **Provider:** self-built in `on_start` from a merged cfg — `Compactor` block keys
  `model` (default `Session.model`), `summary_char_limit` (default **4000** ≈ 1k tokens),
  `timeout_s` (default 60), with `endpoint`/`api_key_env`/`price_per_mtok` inherited from
  the Session block (the AutoExtract 2f pattern; bare `Module = compactor` reuses the
  Session provider config).
- **Prompt:** system: "You maintain a rolling summary of an ongoing conversation. Merge the
  existing summary with the newly dropped turns into ONE updated summary under
  `summary_char_limit` chars. Keep: decisions, open tasks, user preferences/corrections,
  key facts and file/tool state. Drop: pleasantries, superseded attempts, tool noise."
  user: existing summary + the span digest. A **rolling merge**, not an append — the
  summary self-consolidates like core memory under its cap.
- **Worker capture discipline:** non-owning `provider_`/`bb_` + value copies + `&busy_`;
  posts **`SESSION_SUMMARY`** `{session, upto, text}` (truncated to `summary_char_limit`,
  UTF-8-safe) and `AUX_SPENT_USD` (token delta × `price_per_mtok`) — the LLMModule folds it
  into the budget as it already does for auto-extract. **The worker is throw-wrapped and
  ALWAYS posts a terminal reply** (the tool-offload BG_DONE lesson): success →
  `SESSION_SUMMARY`; any error/throw → **`COMPACT_FAILED`** `{session, upto}` — so the
  Arbiter's `pending_compact_` can never wedge on a silent failure. `busy_` cleared on
  every path.
- **Teardown:** `Agent::compactor` declared with the plain modules (before `executor`) so
  the executor joins the worker while the module is alive — the auto-extract member-order
  rule verbatim.

### 3. Apply — Arbiter, pump thread

Subscribes `SESSION_SUMMARY`. Guards, in order:

1. `session` equals the CURRENT session id (a late worker from before a `/new` or
   rotation must not contaminate the fresh session — the cross-turn id-guard lesson).
2. `upto > summarized_upto_` (monotonic; duplicates/stale ignored).

Then: hold `summary_text_` in memory, set `summarized_upto_ = upto`, clear
`pending_compact_`, write the sidecar **atomically** (tmp+rename, parent dir created):

```
upto: <N>

<markdown summary text>
```

and post **`COMPACTED`** `{upto, chars}` (observability — `hades-scope`; StatusModule may
consume later).

`COMPACT_FAILED` (matching session id) just clears `pending_compact_` — the next turn's
detection re-fires with the same (or grown) span. The fold reads `summary_text_` from
MEMORY, not the sidecar — deliberately unlike the per-turn re-read of `memory_file`: the
Arbiter is the single writer here, and the file exists for restart + human inspection, not
as a live edit surface.

### The fold

In `start_turn()`, right AFTER the core-memory fold and BEFORE the skills fold
(conversational context outranks tooling): when `summary_text_` is non-empty append

```
Earlier in this session (compacted from turns no longer shown; may be stale — re-verify
files/live state before relying on a past action's result):
<summary_text_>
```

— the memory-injection-framing lesson applied from day one. Empty → no block.

## Lifecycle

- **`--resume`:** `load_history()` also loads the same-id sidecar (tolerant parse: first
  line `upto: N` — missing/garbage → treat as no summary) into `summary_text_` +
  `summarized_upto_`. An `upto` larger than the loaded history size → clamp to 0 and
  ignore the text (corrupt pairing; self-heals on next drop).
- **`/new` (NEW_SESSION):** `summary_text_.clear()`, `summarized_upto_ = 0`,
  `pending_compact_ = false`; the rotated id means a late `SESSION_SUMMARY` fails guard 1.
  No sidecar is deleted (the old session keeps its pair on disk).
- **Window overlap:** with a fixed budget the window start is not strictly monotonic — a
  run of small recent messages can dip the suffix back below `summarized_upto_`, briefly
  duplicating summarized content in context. Benign, documented, not fought.
- **Origins:** heartbeat/peer-origin turns compact identically — it is the agent's own
  session state, not a per-origin power.
- **Interplay:** auto-extract keeps harvesting durable FACTS per turn (archival); the
  summary carries session NARRATIVE (what we were doing, decisions, open threads). The two
  overlap by design and serve different recalls. Embeddings still exclude the live session;
  nothing changes there.

## Config

```
Module = compactor
Compactor
{
  model              = <aux model>   # default: Session.model
  summary_char_limit = 4000          # rolling summary cap (chars; ≈1k tokens in every turn)
  timeout_s          = 60
}
```

All keys optional; the block may be absent entirely (bare `Module = compactor`).
`dev.hades` ships the module + block COMMENTED (public template default = today's
behavior). `docs/manifest-reference.md` gains a numbered `Compactor` section + a §2 roster
row + a Session-table cross-reference on `history_budget_chars`.

## New bus keys

`COMPACT_REQUEST` `{session, upto, span, current_summary}` · `SESSION_SUMMARY`
`{session, upto, text}` · `COMPACT_FAILED` `{session, upto}` · `COMPACTED` `{upto, chars}`.

## Non-goals / v2 seams (recorded, not built)

Manual `/compact` REPL command · proactive pre-drop compaction (~80% threshold) ·
compaction-triggered auto-extract sweep · per-origin summary policy · summarizing the
sidecar corpus into embeddings · multi-session interplay (backlog item 3).

## Testing

- Pure: span digest builder (truncation walk-back, payload cap, omission marker), summary
  prompt builder, sidecar parse/serialize round-trip (incl. garbage first line).
- Module (FakeProvider, no executor → inline): rolling merge input shape, cap enforcement,
  busy skip, error swallow + busy clear, AUX_SPENT_USD delta.
- Arbiter: detection fires once per drop (no re-fire while pending), COMPACT_REQUEST
  payload correctness (span boundaries, digesting), SESSION_SUMMARY guards (wrong id,
  stale upto), fold placement (after memory, before skills) + label, sidecar written
  atomically, `/new` reset + late-result rejection, `--resume` sidecar reload + clamp,
  **no-module lock: roster without compactor → LLM_REQUEST byte-identical to today**.
- Both sanitizer lanes (ASan+UBSan and TSan — the worker is new cross-thread traffic).

## Pieces

`src/apps/compactor/compactor.cpp` + `include/hades/module/compactor_module.h` (module,
prompt/digest helpers header-inline where pure) · `src/apps/arbiter/arbiter.cpp` +
`include/hades/arbiter.h` (window-start refactor, detect/apply/fold, sidecar io, resume/new
hooks) · `app/agent_wiring.{h,cpp}` (factory, `Compactor` merged cfg, member order,
executor) · `manifests/dev.hades` (commented) · `docs/manifest-reference.md` ·
`prompts/soul.md` (one line: the compacted block is your own earlier conversation) ·
`CLAUDE.md` · tests as above.
