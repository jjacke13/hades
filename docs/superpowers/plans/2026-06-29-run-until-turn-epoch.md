# run_until turn-epoch + race-free budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. **Implementers run on OPUS** (per `feedback_sdd_implementer_opus`). Concurrency-adjacent — read current files fully before editing.

**Goal:** Close the worker-offload `run_until` follow-up: a long-but-progressing turn no longer false-times-out; a stale (timed-out) turn's `LLM_RESPONSE` is dropped instead of answering the wrong prompt; and the `spent_` budget counter can never race. Design: `docs/superpowers/specs/2026-06-29-run-until-turn-epoch-design.md` (read first).

**Architecture:** (1) Arbiter stamps each `LLM_REQUEST` with a per-user-turn epoch; LLMModule echoes it into `LLM_RESPONSE`; Arbiter drops stale-epoch responses. (2) The offloaded worker stops touching module state — budget accrues on the pump thread (post-a-delta). (3) `pump()` returns a dispatched count and `run_until`'s timeout becomes an idle timeout (reset on progress).

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest. Build/test ONLY inside `nix develop`.

## Global Constraints

- **C++20**, g++ 15.2. Every build/test command runs inside `nix develop`.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Invariant preserved:** subscriber handlers run ONLY on the pump thread; workers call `post()` only. After this change the worker touches strictly `provider_->complete(req)` + `bb_->post(LLM_RESPONSE)` — nothing on the module.
- **Backward-compatible:** all 127 existing tests stay green. `pump()` gains a return value old callers ignore; `epoch` defaults to 0; the inline LLM path stays byte-identical in observable order (`LLM_RESPONSE` before `BUDGET_SPENT_USD`).
- TSan pass recommended on T2 (the budget race) + the offload tests.

## File Structure

```
include/hades/llm/provider.h        T1 (modify)  add `std::uint64_t epoch` to LlmRequest
include/hades/arbiter.h             T1 (modify)  turn_epoch_ member
src/arbiter/arbiter.cpp             T1 (modify)  ++epoch on USER_MESSAGE; tag LLM_REQUEST; drop stale LLM_RESPONSE
src/module/llm_module.cpp           T1 (modify)  read req epoch, echo into LLM_RESPONSE
include/hades/blackboard.h          T3 (modify)  pump() returns std::size_t
src/core/blackboard.cpp             T2/T3 (modify) budget-on-pump-thread support touches nothing here; run_until reset-on-progress
src/module/llm_module.cpp           T2 (modify)  worker posts LLM_RESPONSE only; LLMModule accrues spent_ on pump thread
tests/test_arbiter.cpp              T1 (extend)
tests/test_llm_module.cpp           T1/T2 (extend)
tests/test_blackboard.cpp           T3 (extend)
```

---

## Task 1: Turn epoch — tag, echo, drop stale

**Files:** Modify `include/hades/llm/provider.h`, `include/hades/arbiter.h`, `src/arbiter/arbiter.cpp`, `src/module/llm_module.cpp`. Extend `tests/test_arbiter.cpp`, `tests/test_llm_module.cpp`. **Read `src/arbiter/arbiter.cpp` (the USER_MESSAGE / start_turn / on_llm_response / on_tool_result flow) and `src/module/llm_module.cpp` (the LLM_REQUEST handler) fully first.**

**Interfaces — Produces:** `LlmRequest.epoch`; `LLM_REQUEST` payload carries `"epoch"`; `LLM_RESPONSE` payload echoes `"epoch"`; Arbiter ignores stale-epoch responses.

- [ ] **Step 1: Failing tests.**
  - `tests/test_arbiter.cpp` — `StaleEpochResponseIsDropped`: `a.on_attach(bb)`; subscribe to `ASSISTANT_MESSAGE` (capture) and `LLM_REQUEST`. Post `USER_MESSAGE "first"`, pump (turn epoch 1, an LLM_REQUEST emitted with epoch 1). Post `USER_MESSAGE "second"`, pump (epoch 2). Now post a **stale** `LLM_RESPONSE{{"text","late answer"},{"epoch",1}}`, pump → assert NO `ASSISTANT_MESSAGE == "late answer"` (dropped). Then post `LLM_RESPONSE{{"text","real"},{"epoch",2}}`, pump → assert `ASSISTANT_MESSAGE == "real"`. (Read how the existing arbiter tests post LLM_RESPONSE and adapt; assert the LLM_REQUESTs carried increasing `epoch`.)
  - `tests/test_llm_module.cpp` — `EchoesRequestEpoch`: post `LLM_REQUEST{{"messages",[]},{"tools",[]},{"epoch",7}}`, pump, capture `LLM_RESPONSE` → `EXPECT_EQ(e.value.value("epoch", 0ull), 7ull)` (inline path via the StubProvider).
- [ ] **Step 2: Run, expect FAIL** (epoch unknown / not echoed / not dropped).
- [ ] **Step 3: Implement.**
  - `provider.h`: add `std::uint64_t epoch = 0;` to `LlmRequest` (include `<cstdint>`).
  - `arbiter.h`: add `std::uint64_t turn_epoch_ = 0;` (include `<cstdint>`).
  - `arbiter.cpp`: in the `USER_MESSAGE` handler, `++turn_epoch_;` before `start_turn()`. In `start_turn()`, add `{"epoch", turn_epoch_}` to the `LLM_REQUEST` payload. In `on_llm_response`, read `const auto ep = e.value.value("epoch", 0ull);` and **return early (drop, optionally log to Eventlog via a posted note)** if `ep != turn_epoch_`. (Tool-loop continuations re-`start_turn()` with the same unchanged `turn_epoch_`, so mid-turn responses are NOT dropped.)
  - `llm_module.cpp`: read `req.epoch` from the `LLM_REQUEST` entry (`e.value.value("epoch", 0ull)`), set it on the `LlmRequest`, and include `{"epoch", req.epoch}` in the `LLM_RESPONSE` payload (both inline and worker paths — build it into the `run`/response builder so both inherit it).
- [ ] **Step 4: Build + test** `-R "Arbiter|LLMModule"`, then full suite green.
- [ ] **Step 5: Commit** `feat: turn-epoch on LLM_REQUEST/RESPONSE; Arbiter drops stale-epoch responses`

---

## Task 2: Race-free budget — worker posts only; accrue `spent_` on the pump thread

**Files:** Modify `src/module/llm_module.cpp` (and `include/hades/module/llm_module.h` if a handler/member is added). Extend `tests/test_llm_module.cpp`. **Read the current `run`/offload code from Task 1's state first.**

**Interfaces — Produces:** the offloaded worker mutates no module state; `spent_`/`BUDGET_SPENT_USD` accrue on the pump thread; observable order (`LLM_RESPONSE` then `BUDGET_SPENT_USD`) preserved.

- [ ] **Step 1: Failing/again-green test.** Add `BudgetAccruesOnPumpThreadUnderOffload` to `tests/test_llm_module.cpp`: with an `Executor` + a provider returning known token counts (e.g. 1e6 prompt tokens, price 2.0), post `LLM_REQUEST`, `bb.run_until([&]{return budget_seen;}, 5.0)`; assert `BUDGET_SPENT_USD == 2.0` and that it arrives (the worker no longer posts it directly — the pump-thread handler does). Keep the existing `AnswersRequestAndTracksBudget` (inline) green. (Order the test locals so the `Executor` is destroyed before the module/Blackboard — as in the existing offload test — to avoid the Task-3-of-worker-offload UAF.)
- [ ] **Step 2: Run, expect FAIL** (if the refactor isn't in place the worker still mutates `spent_`, or budget posts from the wrong thread).
- [ ] **Step 3: Implement.** Split the response handling: the worker closure does ONLY `LlmResponse r = provider_->complete(req); bb_->post("LLM_RESPONSE", {…text/tool_call/tokens/stop_reason/epoch…})` — **remove the `spent_ += …` and the `BUDGET_SPENT_USD` post from the worker.** Have the `LLMModule` accrue budget on the pump thread: subscribe (in `on_attach`) to `LLM_RESPONSE` and, in that handler (pump thread), read `prompt_tokens`/`completion_tokens`, do `spent_ += (p+c)/1e6 * price_per_mtok_`, and `post("BUDGET_SPENT_USD", spent_)`. For the **inline** path, either route through the same handler (preferred — single code path) or keep the direct post; whichever, preserve the observable `LLM_RESPONSE`-before-`BUDGET_SPENT_USD` order and the existing test's exact value. Update/clarify the `spent_` comment: now written only on the pump thread → race-free regardless of overlap.
- [ ] **Step 4: Build + test** `-R LLMModule`, full suite green. **TSan pass** over the LLMModule + offload e2e tests (the budget race is the headline — prove it's gone).
- [ ] **Step 5: Commit** `fix: accrue LLM budget on the pump thread (worker posts only) — race-free spent_`

---

## Task 3: Deadline-on-progress — `run_until` idle timeout

**Files:** Modify `include/hades/blackboard.h`, `src/core/blackboard.cpp`. Extend `tests/test_blackboard.cpp`. **Read the current `pump()` and `run_until` first.**

**Interfaces — Produces:** `std::size_t Blackboard::pump()` (was `void`); `run_until` resets its deadline whenever a pump delivered ≥1 entry (idle timeout).

- [ ] **Step 1: Failing tests** (append to `tests/test_blackboard.cpp`):
  - `PumpReturnsDispatchCount`: subscribe to `"X"`; `post("X",1,"s"); post("X",2,"s");` → `EXPECT_EQ(bb.pump(), 2u);` and a later `pump()` on an empty queue returns 0.
  - `RunUntilIdleTimeoutResetsOnProgress`: a worker thread posts a `"TICK"` every ~40 ms for ~0.4 s (subscribed to bump a counter), while the main thread runs `bb.run_until([]{return false;}, 0.2)` (predicate never true). Assert it returns `false` only **after** the ticks stop (elapsed ≳ 0.4 s + ~0.2 s idle), NOT at 0.2 s — i.e. progress reset the deadline. Use a steady_clock measurement around the call with generous bounds (assert elapsed > 0.4 s). `worker.join()` before asserting.
  - Keep `RunUntilTimesOutWhenPredicateNeverHolds` (no progress → times out ~0.2 s) green.
- [ ] **Step 2: Run, expect FAIL** (pump returns void; deadline not reset).
- [ ] **Step 3: Implement.** `blackboard.h`: change `void pump();` → `std::size_t pump();`. `blackboard.cpp`: `pump()` counts entries it dispatches (one per dequeued entry that matched ≥0 subs — count dequeued entries, or count handler invocations; pick "entries dequeued" so a delivered-but-unmatched event still counts as progress) and returns the count. `run_until`: after `pump()`, `if (dispatched > 0) deadline = now() + timeout_s;` (reset the idle window on progress). Preserve the existing inline-completion fast path and the spurious-wakeup-safe cv wait. **Do not** hold `mu` across `pump()`/`done()`.
- [ ] **Step 4: Build + test** `-R Blackboard`, full suite green. (TSan pass on the new progress test recommended.)
- [ ] **Step 5: Commit** `feat: pump() returns dispatch count; run_until uses an idle (reset-on-progress) timeout`

---

## Self-Review (against the spec)

- **Coverage:** epoch tag+echo+drop (T1) → stale-response leak fixed; post-a-delta budget on pump thread (T2) → `spent_` race fixed; idle timeout (T3) → false-timeout on long turns fixed. All three review defects (a/b/c) closed.
- **Invariant:** the worker now touches only `provider_` + `bb_->post`; all module-state writes on the pump thread. Strictly tighter than worker-offload shipped.
- **Backward-compat:** epoch defaults 0; `pump()` return ignored by old callers; inline budget order preserved; 127 tests green.
- **Type consistency:** `epoch` is `std::uint64_t` everywhere (provider.h, arbiter.h, the JSON `value<uint64_t>`); `pump()` returns `std::size_t`.

## Verification

1. Full suite green. 2. TSan over T2 + offload e2e (budget race gone) and T3 progress test. 3. Manual (needs key): a multi-step turn that would exceed 180 s no longer times out; a killed/hung turn still times out and the next turn is unaffected.
