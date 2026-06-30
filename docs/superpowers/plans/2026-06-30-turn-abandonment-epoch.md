# Turn-abandonment signal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. **Implementers run on OPUS** (per `feedback_sdd_implementer_opus`). Read current files fully before editing.

**Goal:** Close the epoch dispatch-ordering hole robustly: a turn abandoned on `run_until` timeout posts `TURN_ABANDONED`; the Arbiter bumps `turn_epoch_` on it so a stale (post-abandonment) `LLM_RESPONSE` is dropped instead of answering the next prompt. Enable the `DISABLED_` regression test.

**Architecture:** Bus-decoupled. Front-ends (only actors that observe a timeout) post `TURN_ABANDONED` when `run_until` returns false; the Arbiter (sole owner of `turn_epoch_`) handles it. At timeout the queue is empty (idle timeout = no activity for N s), so `TURN_ABANDONED` is enqueued ahead of any later stale worker response → the existing epoch guard drops it. Spec: `docs/superpowers/specs/2026-06-30-turn-abandonment-epoch-design.md` (read first).

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest. Build/test ONLY inside `nix develop`.

## Global Constraints

- **C++20**, g++ 15.2. Every build/test command runs inside `nix develop`.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Bus-decoupled:** no module calls another directly; `TURN_ABANDONED` is a Blackboard message (a new key).
- **Additive / backward-compat:** a turn that completes normally never posts `TURN_ABANDONED`. All 165 existing tests stay green. The Arbiter's existing epoch-drop logic is unchanged — this only adds a second epoch-bump trigger.
- **IMPLEMENTER DISCIPLINE:** you MUST build + run the FULL suite + `git commit` + verify `git log --oneline -1` + write the report, then reply. Do NOT report DONE unless `git log` shows your commit (prior implementers flaked on the commit step — do not repeat).

## File Structure

```
src/arbiter/arbiter.cpp        T1 (modify)  subscribe TURN_ABANDONED -> ++turn_epoch_ + clear pending
include/hades/arbiter.h        T1 (maybe)   (no new member likely needed; turn_epoch_ exists)
tests/test_arbiter.cpp         T1 (modify)  enable+rewrite the DISABLED_ test
src/module/chat_module.cpp     T2 (modify)  post TURN_ABANDONED + print [timed out] on run_until==false
src/module/http_server_module.cpp T2 (modify) post TURN_ABANDONED on run_until==false ([timed out] reply kept)
tests/test_chat.cpp / tests/test_serve.cpp  T2 (modify)  timeout -> TURN_ABANDONED posted
```

---

## Task 1: Arbiter handles `TURN_ABANDONED` (+ enable the regression test)

**Files:** Modify `src/arbiter/arbiter.cpp` (and `include/hades/arbiter.h` only if a member is needed — likely not), `tests/test_arbiter.cpp`. **Read `src/arbiter/arbiter.cpp` fully first** — the `on_attach` subscriptions, `turn_epoch_`, the `USER_MESSAGE` handler (`++turn_epoch_`), `on_llm_response` (the `if (ep != turn_epoch_) drop` guard), and `pending_`/`pending_msg_`.

**Interfaces — Produces:** the Arbiter subscribes to `TURN_ABANDONED`; on it, `++turn_epoch_` and clears `pending_`/`pending_msg_`.

- [ ] **Step 1: Failing test.** In `tests/test_arbiter.cpp`, ENABLE + rewrite the existing `DISABLED_StaleResponseDispatchedBeforeNextUserMessageIsAccepted` into an active `StaleResponseAfterAbandonmentIsDropped` (remove the `DISABLED_` prefix; update the comment to describe the abandonment model). Body:
  - `Arbiter a; a.on_attach(bb);` subscribe captures for `ASSISTANT_MESSAGE` (text) + `LLM_REQUEST`.
  - `bb.post("USER_MESSAGE","first","chat"); bb.pump();` → turn epoch 1; an LLM_REQUEST{epoch 1} emitted; do NOT answer it.
  - `bb.post("TURN_ABANDONED", nlohmann::json::object(), "chat"); bb.pump();` → epoch bumped to 2.
  - `bb.post("LLM_RESPONSE", {{"text","late answer for turn 1"},{"epoch",1}}, "llm"); bb.pump();` → assert NO `ASSISTANT_MESSAGE == "late answer for turn 1"` (dropped: 1 != 2).
  - `bb.post("USER_MESSAGE","second","chat"); bb.pump();` → epoch 3; `bb.post("LLM_RESPONSE", {{"text","real"},{"epoch",3}}, "llm"); bb.pump();` → assert `ASSISTANT_MESSAGE == "real"`.
  - (Read the epoch the Arbiter actually stamps on its LLM_REQUESTs from the captured requests rather than hard-assuming 1/2/3 if the numbering differs — but with `++turn_epoch_` per USER_MESSAGE and per TURN_ABANDONED, first=1, after-abandon=2, second=3; verify against captured `LLM_REQUEST["epoch"]`.)
  - Add a second assertion: a `TURN_ABANDONED` clears a pending confirm — drive a confirm-gated tool (reuse the existing `DestructiveToolCallIsConfirmGated` setup), then `TURN_ABANDONED`, then a `CONFIRM_RESPONSE{approved:true}` for the abandoned action → assert the tool is NOT executed (pending was cleared). (If wiring this is fiddly, at minimum assert `pending_` cleared via observable behavior; keep it real, not tautological.)
- [ ] **Step 2: Run, expect FAIL** — `TURN_ABANDONED` is unhandled, so the stale `LLM_RESPONSE{1}` is accepted (epoch still 1) → "late answer for turn 1" reaches ASSISTANT_MESSAGE → assertion fails.
- [ ] **Step 3: Implement.** In `arbiter.cpp` `on_attach`, add `bb.subscribe("TURN_ABANDONED", [this](const Entry&){ ++turn_epoch_; pending_ = nullptr; pending_msg_ = nullptr; });` (match the actual field names/types — `pending_`/`pending_msg_` may be `nlohmann::json`; set them to `nullptr`/`json()` as the existing `on_confirm` reset does). No change to `on_llm_response`'s drop guard. (No header change unless a helper is added.)
- [ ] **Step 4: Build + test** `-R Arbiter`, then FULL suite green (165 active, the formerly-disabled test now ACTIVE → count of active tests +1).
- [ ] **Step 5: Commit** `feat: Arbiter bumps turn-epoch on TURN_ABANDONED (drops stale post-abandonment LLM_RESPONSE)`

---

## Task 2: Front-ends post `TURN_ABANDONED` on `run_until` timeout

**Files:** Modify `src/module/chat_module.cpp`, `src/module/http_server_module.cpp`, and the matching tests (`tests/test_chat.cpp` and/or `tests/test_serve.cpp`). **Read each fully first** — in `chat_module.cpp` the `run_repl` + `run_repl_readline` loops (`run_until(turn_done_, kTurnTimeoutS)`); in `http_server_module.cpp` the `collect_` (`run_until(... , kCollectTimeoutS)` + the `[timed out]` reply branch).

**Interfaces — Produces:** on `run_until` returning false, the front-end posts `TURN_ABANDONED` and surfaces a timeout to the user.

- [ ] **Step 1: Failing tests.**
  - `tests/test_chat.cpp` — a turn that never completes (no handler turns USER_MESSAGE into ASSISTANT_MESSAGE) with a SMALL timeout: drive `run_repl` with one input line, subscribe to `TURN_ABANDONED` → assert it was posted AND the output contains `[timed out]`. (Use the injected `istream`/`ostream` seam; pass a small timeout — see note below on how the timeout is configured/overridable for the test.)
  - `tests/test_serve.cpp` — `collect_` (or the public `handle_chat`/equivalent) with no completing turn + small timeout → the JSON reply is `[timed out]` AND `TURN_ABANDONED` was posted.
  - **Timeout for tests:** `kTurnTimeoutS`/`kCollectTimeoutS` are 180s — too long for a test. Read how the test would otherwise force a timeout; if there is no seam, add a minimal one (e.g. an optional ctor/param or a `set_turn_timeout(double)` used only by tests, defaulting to 180s) so the test can set ~0.05s. Keep production default 180s. (If a seam already exists, use it. Match the existing test style.)
- [ ] **Step 2: Run, expect FAIL** — no `TURN_ABANDONED` posted today; REPL prints nothing on timeout.
- [ ] **Step 3: Implement.**
  - `chat_module.cpp` `run_repl` + `run_repl_readline`: capture `bool ok = bb_->run_until([this]{ return turn_done_; }, timeout);` — if `!ok`: `bb_->post("TURN_ABANDONED", nlohmann::json::object(), "chat"); bb_->pump();` and print `assistant> [timed out]` (via the existing colored-label path / `out_`). Then continue the loop. (Pump once after posting so the Arbiter processes the abandonment before the next user line is read — keeps the epoch bumped promptly; harmless if empty.)
  - `http_server_module.cpp` `collect_`: in the `run_until` false / `[timed out]` branch, also `bb_->post("TURN_ABANDONED", nlohmann::json::object(), "serve"); bb_->pump();` before returning the `[timed out]` reply. (Whole turn already under `mu_`; the post+pump stays under it.)
  - Use the test-overridable timeout from Step 1.
- [ ] **Step 4: Build + FULL suite** — all green (existing test_chat echo turn completes inline → `run_until` returns true → no TURN_ABANDONED, unchanged; existing test_serve likewise).
- [ ] **Step 5: Commit** `feat: front-ends post TURN_ABANDONED + surface [timed out] when a turn times out`

---

## Self-Review (against the spec)

- **Coverage:** Arbiter handles TURN_ABANDONED (T1); front-ends emit it on timeout (T2); the formerly-DISABLED hole is now an ACTIVE passing test. The dispatch-ordering hole is closed independent of timing.
- **Out of scope honored:** no TOOL_RESULT epoch-stamping (tools sync today — note left in code/spec), no history rewind of the abandoned turn.
- **Backward-compat:** additive; normal turns never abandon; 165 tests green (the one disabled test becomes active).
- **Type consistency:** `turn_epoch_` is `std::uint64_t`; `TURN_ABANDONED` payload is an (empty) json object; pending fields reset exactly as `on_confirm` does.

## Verification

1. Full suite green (the DISABLED test now active + the two front-end timeout tests). 2. Manual (needs key): force a hung turn (or a tiny timeout) → REPL prints `[timed out]`, a subsequent prompt is answered correctly (no wrong-prompt carryover). 3. Confirm a normal fast turn never emits TURN_ABANDONED (no `[timed out]` spam).
