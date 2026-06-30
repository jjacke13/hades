# Turn-abandonment signal — close the epoch dispatch-ordering hole — design spec

**Date:** 2026-06-30
**Status:** approved (Vaios: "if there isn't a simpler solution, plan it properly and do it" — confirmed no
simpler robust option; the abandonment signal is irreducible). Ready for plan.
**Closes:** the residual from the worker-offload `run_until` follow-up (the `DISABLED_` test
`Arbiter.DISABLED_StaleResponseDispatchedBeforeNextUserMessageIsAccepted`).

## The hole (recap)

The Arbiter holds `turn_epoch_`, bumped only when a `USER_MESSAGE` is **dispatched** (on the pump thread).
The front-ends do `post(USER_MESSAGE); run_until(turn_done, timeout)`. If a turn times out (is abandoned)
while its offloaded LLM worker is still in flight, that worker later posts `LLM_RESPONSE{epoch=E}`. If that
stale response sits in the queue **ahead** of the next `USER_MESSAGE{→E+1}`, `pump()` dispatches it while
`turn_epoch_` is still E → `ep == turn_epoch_` passes → the Arbiter answers the WRONG (previous) prompt.

Unreachable in the shipped binary because the idle timeout (180s) > max single in-flight poster (cpr 120s +
tool 30s), so when `run_until` abandons a turn no worker is still running. But future SSE / tool-offload /
configurable timeouts break that invariant. We close it now, robustly.

## Why no simpler solution exists (the irreducibility argument)

A purely Arbiter-side fix cannot work: a turn's **slow-but-legitimate** response and its
**stale-after-abandonment** response are byte-identical to the Arbiter (`epoch == current`, a request was
outstanding). The ONLY thing that distinguishes them is whether the front-end gave up waiting — which the
Arbiter never observes (it has no timer; it is purely reactive to the bus). An "awaiting-response latch" or
"outstanding-request-epoch" check accepts the stale response in exactly the abandoned-mid-call case, because
the Arbiter legitimately *was* awaiting that epoch's response. Therefore the fix REQUIRES an explicit
abandonment signal originating from the front-end (the only actor that knows a turn timed out).

## The fix — a `TURN_ABANDONED` bus message

Two small changes, fully bus-decoupled (no module calls another directly):

### 1. Front-ends signal abandonment (`chat_module.cpp`, `http_server_module.cpp`)
When `run_until(...)` returns **false** (idle timeout — the turn is abandoned):
- post `TURN_ABANDONED` (no payload needed; optionally the abandoned `turn_epoch_` is not known to the
  front-end, so payload is empty / a monotonic marker).
- surface it to the user: REPL prints `assistant> [timed out]` (today it silently loops); HTTP `collect_`
  already returns `{"reply":"[timed out]"}` — keep that, just add the `TURN_ABANDONED` post.

**Ordering correctness:** the actual guarantee is sequencing, not an empty-queue claim. The front-end
posts `TURN_ABANDONED` AND pumps it (so the Arbiter bumps its epoch) BEFORE it reads/accepts the next
user input. Therefore any later stale worker `LLM_RESPONSE{E}` — even one that raced into the queue during
`run_until`'s final ~20 ms `cv.wait_for` (which can wake the predicate and still return false with that
entry queued) — is stamped with the now-superseded epoch and is dropped by the existing epoch check before
it can contaminate a SUBSEQUENT prompt. The one harmless residual: a response that raced in just before
abandonment may still be *delivered late against its OWN turn* and print after `[timed out]` — purely
cosmetic, and unreachable today because the idle timeout is held strictly greater than the maximum single
in-flight poster duration (so no worker is still running when `run_until` abandons). Robust regardless of
how long the worker runs.

### 2. Arbiter handles `TURN_ABANDONED` (`arbiter.cpp`)
Subscribe to `TURN_ABANDONED`. On it:
- `++turn_epoch_;` — invalidates any in-flight/stale response from the abandoned turn.
- clear `pending_` / `pending_msg_` (a confirm-gated action from the abandoned turn must not survive into a
  new turn). (Confirms are read inline today so this is belt-and-suspenders, but correct.)
- (no history mutation — the abandoned user message stays in `history_`; v1 keeps it. A future "rewind
  abandoned turn from history" is out of scope — see below.)

After this, the existing `on_llm_response` epoch guard (`if (ep != turn_epoch_) drop`) does the actual
dropping; no change to that logic. The next `USER_MESSAGE` bumps the epoch again as today.

## Data flow (abandoned turn, with the fix)

```
USER_MESSAGE → ++turn_epoch_=E, LLM_REQUEST{E} → worker (slow) …
run_until(idle): … no activity for timeout_s … returns FALSE
  front-end: post TURN_ABANDONED ; print/return "[timed out]"      [queue was empty → ABANDONED first]
  … worker finally finishes → post LLM_RESPONSE{E}                 [lands AFTER TURN_ABANDONED]
next turn: post USER_MESSAGE ; run_until → pump:
   TURN_ABANDONED → ++turn_epoch_=E+1, clear pending
   LLM_RESPONSE{E} → E != E+1 → DROPPED (existing guard)           ← the bug, now closed
   USER_MESSAGE → ++turn_epoch_=E+2 → start turn                   → correct, fresh
```

## Scope / out of scope

- **In scope:** `TURN_ABANDONED` for offloaded `LLM_RESPONSE` (the documented residual). Enable + rewrite the
  `DISABLED_` Arbiter test to model abandonment.
- **Out of scope (noted):** epoch-stamping `TOOL_REQUEST`/`TOOL_RESULT` — tools run SYNCHRONOUSLY today
  (`run_subprocess` blocks the pump thread, so a turn cannot time out mid-tool, so no stale `TOOL_RESULT`
  exists). When tool-offload lands, the SAME epoch+abandonment pattern must extend to `TOOL_RESULT` — call
  this out in code + the tool-offload spec. Rewinding the abandoned user message from `history_` (so the
  transcript doesn't carry a dead turn) is a separate UX decision, deferred.

## Testing (TDD)

- `test_arbiter` — ENABLE + rewrite `DISABLED_StaleResponseDispatchedBeforeNextUserMessageIsAccepted` →
  `StaleResponseAfterAbandonmentIsDropped`: `USER_MESSAGE`(epoch 1) → `TURN_ABANDONED` → stale
  `LLM_RESPONSE{epoch:1}` → `USER_MESSAGE`(epoch 2) → fresh `LLM_RESPONSE{epoch:2}` → assert the stale one
  produced NO assistant/tool action and the fresh one DID. Also: `TURN_ABANDONED` clears a `pending_`
  confirm.
- `test_chat` / `test_serve` — a front-end whose `run_until` times out (inject a never-completing turn /
  zero-ish timeout) posts `TURN_ABANDONED` and surfaces `[timed out]`. (Use the injected-stream / scripted
  seam; keep offline + fast.)
- Existing 165 tests stay green (TURN_ABANDONED is additive; a turn that completes normally never posts it).

## MOOS-IvP framing

`TURN_ABANDONED` is the helm declaring a command **aborted** — once aborted, any late actuator feedback for
that command is rejected, exactly as a helm discards a stale sensor return for a maneuver it already gave up
on. It keeps the single deterministic decision thread as the sole authority on "which command is live."
