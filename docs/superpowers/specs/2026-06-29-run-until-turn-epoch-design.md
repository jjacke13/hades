# run_until turn-epoch + race-free budget — design spec (worker-offload follow-up)

**Date:** 2026-06-29
**Status:** approved-in-direction (Vaios chose "build next: turn-epoch + in-flight guard") → this spec
**refines** the budget half (post-a-delta supersedes a bare in-flight guard — see §Decision). Ready for plan.
**Precedes:** SSE/WebSocket streaming and the agent↔agent Bridge (both build on `run_until`'s timeout semantics).

## The problem (from the worker-offload final whole-branch review)

After worker-offload shipped, the LLM HTTP call runs on an `Executor` worker that `post()`s `LLM_RESPONSE`
back; the front-ends drive a turn with `bb_->run_until(pred, 180.0)`. Three coupled defects, all gated
behind the 180 s timeout:

1. **The deadline is fixed at `run_until` entry and never reset per event.** A *legitimate* long
   multi-step turn can exceed 180 s with nothing hung: each cpr LLM call is capped at 120 s
   (`http_cpr.cpp`) and the Arbiter allows up to 25 tool steps (`arbiter.cpp`). So a progressing turn can
   trip the timeout.
2. **Stale-response leak.** On timeout the front-end abandons the turn (returns to the prompt / sends
   `[timed out]`) while a worker is still in flight. That worker's `LLM_RESPONSE` is dequeued on the
   **next** turn's first `pump()` → the Arbiter answers the *previous* prompt → a one-off cascading
   wrong-prompt desync.
3. **`spent_` data race.** If a new turn starts while the timed-out turn's worker is still running,
   `start_turn` submits a **second** LLM task → two workers do the non-atomic `spent_ += …`
   concurrently → UB / corrupted budget counter. TSan never caught it because no test reaches 180 s.

Consequence is non-catastrophic (wrong-prompt answer + a corrupted budget number; no UAF/crash/security)
and the path is opt-in — which is why it didn't block worker-offload's merge. But it is the seam SSE and
the Bridge will build on, so it is fixed first.

## Decision — what we build (and why it refines the chosen approach)

Vaios chose **turn-epoch + in-flight guard**. This spec keeps the **turn-epoch** (it is the right fix for
the stale-response leak) and **replaces the bare in-flight guard with the design's own pre-recorded
race fix: post-a-delta budget accumulation on the pump thread.** Reasoning:

- An in-flight guard ("don't start a new turn while a worker is outstanding") would prevent the *overlap*,
  but it does **not** keep the budget accurate (the timed-out call really did spend tokens) and it adds a
  blocking/▢-state to the front-ends. The post-a-delta scheme removes the race at its root — the worker
  stops mutating module state entirely — and **the turn-epoch already makes a separate guard
  unnecessary** for correctness: a stale in-flight response is dropped by epoch mismatch, and with budget
  on the pump thread, two overlapping calls can no longer race. So `epoch + post-a-delta` is strictly
  more robust than `epoch + guard`, with less front-end complexity.

Three changes (each independently testable):

### 1. Turn epoch — Arbiter drops stale `LLM_RESPONSE` (fixes the leak)

- The `Arbiter` holds `std::uint64_t turn_epoch_ = 0`, **incremented on each new `USER_MESSAGE`** (the
  start of a user turn). Tool-loop continuations within a turn keep the same epoch.
- Every `LLM_REQUEST` the Arbiter posts carries `"epoch": turn_epoch_`.
- The `LLMModule` **echoes** the request's epoch into the `LLM_RESPONSE` it posts (copy `req.epoch` →
  response payload). Works on both paths (inline + offloaded worker).
- `Arbiter::on_llm_response` **ignores** (drops, with an Eventlog-visible note) any `LLM_RESPONSE` whose
  `epoch != turn_epoch_`. A timed-out turn's late response is discarded, never applied to a newer turn.
- Carry the epoch through the request struct: add `std::uint64_t epoch = 0;` to `LlmRequest`
  (provider.h) — the LLMModule reads it from the `LLM_REQUEST` entry and sets it on the response. (The
  HTTP provider ignores it; it is bus-level metadata, not sent to the LLM API.)

### 2. Race-free budget — post-a-delta, accumulate on the pump thread (fixes the `spent_` race)

- The offloaded worker closure does **only** `provider_->complete(req)` then
  `bb_->post("LLM_RESPONSE", {…, prompt_tokens, completion_tokens, epoch})`. **It no longer touches
  `spent_` / `price_per_mtok_`** — it mutates nothing on the module.
- The `LLMModule` accumulates the budget **on the pump thread**: it subscribes to its own
  `LLM_RESPONSE` and, in that handler (which the contract guarantees runs only on the pump thread),
  computes `spent_ += (prompt+completion)/1e6 * price_per_mtok_` and posts the cumulative
  `BUDGET_SPENT_USD`. Since `spent_` is now written only on the pump thread, there is **no data race**
  even if two LLM workers ever overlap. (The inline path can keep posting budget directly, or route
  through the same handler — the plan picks one; the observable order `LLM_RESPONSE` then
  `BUDGET_SPENT_USD` must be preserved so `StayOnBudget` keeps working.)
- Bonus: this also shrinks the worker's lifetime footprint to `provider_` + `bb_->post` only — a
  strictly simpler teardown story than worker-offload shipped with.

### 3. Deadline-on-progress — `run_until` doesn't abandon a progressing turn (fixes the false timeout)

- `Blackboard::pump()` returns the number of entries it dispatched (`void` → `std::size_t`;
  existing callers that ignore the return are unaffected — backward-compatible).
- `run_until` resets its deadline whenever a `pump()` delivered ≥ 1 entry: the timeout becomes an
  **idle** timeout (no bus activity for `timeout_s`) rather than an absolute one. A turn making steady
  progress (LLM → tool → LLM → …) never trips it; only a genuinely stalled turn does — and if a real
  stall later wakes, change (1) drops its stale response harmlessly.

## Data flow (offloaded turn, after the fix)

```
USER_MESSAGE → Arbiter: ++turn_epoch_, post LLM_REQUEST{…, epoch=E}
  └ LLMModule: executor.submit(λ)                         ── pump thread free
       worker: r = provider.complete(req); post LLM_RESPONSE{…, tokens, epoch=E}   (no spent_ touch)
  run_until(idle 180s): pump (resets deadline on each delivered event) … cv.wait …
       ← LLM_RESPONSE pumped:  • LLMModule handler: spent_ += delta; post BUDGET_SPENT_USD  (pump thread)
                                • Arbiter handler: if epoch==turn_epoch_ → act; else DROP
```

## Error handling / edge cases

- A turn that genuinely hangs > 180 s idle still times out (correct) — and a later stale response is
  dropped by epoch, the budget for it still accounted on the pump thread (accurate).
- `LLM_RESPONSE` with no `epoch` (defensive / older entry) → treated as epoch 0; a live turn's epoch is
  ≥ 1 after the first USER_MESSAGE, so an unstamped response is dropped unless it's the very first — the
  LLMModule always stamps, so this only arises from a malformed external post (acceptable to drop).
- Inline path (no executor, all tests): epoch echo + pump-thread budget both run synchronously on the
  one thread → behavior is byte-identical to a correct turn; the existing budget test still sees
  `BUDGET_SPENT_USD` after `LLM_RESPONSE`.

## Testing (TDD, GoogleTest)

- **Epoch drop** (`test_arbiter`): drive `USER_MESSAGE` (epoch 1) → don't answer; drive a second
  `USER_MESSAGE` (epoch 2); then post a *stale* `LLM_RESPONSE{epoch:1}` → assert the Arbiter does NOT
  emit an ASSISTANT_MESSAGE / TOOL_REQUEST for it (dropped); then a fresh `LLM_RESPONSE{epoch:2}` → acts.
- **Epoch echo** (`test_llm_module`): post `LLM_REQUEST{epoch:7}` → the emitted `LLM_RESPONSE` carries
  `epoch == 7` (inline and, with an Executor, offloaded).
- **Race-free budget** (`test_llm_module`): with an Executor + a slow provider, `spent_`/`BUDGET_SPENT_USD`
  are produced via the pump-thread handler (assert the value is correct after `run_until`); the worker
  closure touches no module field (compile-level: the lambda captures only what it needs). Existing
  budget test (`AnswersRequestAndTracksBudget`) stays green.
- **Deadline-on-progress** (`test_blackboard`): a helper posts an event every ~50 ms from a worker for
  ~0.5 s while the main thread is in `run_until(pred-never-true, 0.2)`; assert it does NOT time out at
  0.2 s because each delivered event resets the idle deadline; then stop posting → it times out ~0.2 s
  after the last event. Plus: `pump()` returns the dispatched count (a small direct assertion).
- Existing 127 tests stay green (epoch defaults, pump() return ignored by old callers, inline budget
  order preserved).
- TSan pass over the offload + new tests (the budget race is the headline — prove it's gone).

## Out of scope (later)

- An explicit in-flight guard / request cancellation (subsumed here; a true cancel of an in-flight cpr
  call is a bigger change, deferred).
- SSE partial-token streaming, tool-call offload, concurrent turns, the Bridge.

## MOOS-IvP framing

The epoch is a **freshness stamp** on helm commands: a late reply computed against a superseded world
state is discarded rather than acted on — the same reason a helm ignores a stale sensor frame. Moving
budget accrual onto the single pump thread keeps the **one deterministic decision thread** as the only
writer of agent state, exactly the invariant worker-offload was built to protect.
