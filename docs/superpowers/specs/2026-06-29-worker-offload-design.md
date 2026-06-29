# Worker-offload bus (concurrency groundwork) — design spec

**Date:** 2026-06-29
**Status:** approved (decision B + groundwork) → ready for plan. **BUILD in a fresh session** (delicate concurrency change; needs full context headroom).

## Goal

Unblock the roadmap (SSE streaming, concurrent tool calls, the agent↔agent Bridge) **without losing the
single-threaded determinism** that is the harness's real asset. Decision **B (worker-offload)**: keep the
**bus single-threaded** — subscriber handlers always run one-at-a-time on the pump thread — but move
**blocking work (LLM HTTP, later tool subprocess) onto worker threads that post their results back** onto
the bus queue. Workers never touch the Blackboard's dispatch; they only `post()` (enqueue). This build is
the **groundwork** + the **LLM offload as proof**; SSE/Bridge build on it later.

**Hard invariant:** subscriber handlers are invoked ONLY on the pump thread (the determinism guarantee).
Worker threads only call `post()` (thread-safe enqueue). All 119 existing tests stay green — offload is
**opt-in** (active only when an `Executor` is set), so the synchronous `pump()`-to-quiescence path is
unchanged for tests and any inline provider.

## Components

### 1. Thread-safe Blackboard + `run_until` event loop — `include/hades/blackboard.h`, `src/core/blackboard.cpp`

The Blackboard's shared state (`queue`, `latest`, `seq`, the Eventlog append) becomes guarded by a
`std::mutex`, with a `std::condition_variable` signalled on every `post()`.

- **`post()`** (now callable from ANY thread — a worker delivering a result): lock → bump `seq`, update
  `latest`, append to the Eventlog, push to `queue` → unlock → `cv.notify_one()`. Thread-safe.
- **`pump()`** (pump thread ONLY — unchanged contract): loop { lock → if `queue` empty, unlock+break →
  pop front → **unlock** → dispatch the entry to matching subscribers }. **Handlers run with the lock
  released** (so a handler's `post()` re-locks; and handlers, running only on the pump thread, stay
  serialized). `min_interval` logic unchanged.
- **`get()`** reads `latest` under the lock.
- **NEW `bool run_until(const std::function<bool()>& done, double timeout_s)`** — the event loop for
  async turns:
  ```
  deadline = now()+timeout_s
  while(!done()){
    pump();                                   // drain current queue (may complete the turn inline)
    if(done()) return true;
    unique_lock lk(mu);
    if(queue empty) cv.wait_for(lk, slice, [&]{ return !queue.empty(); });   // sleep until a worker posts
    if(now() > deadline) return false;        // overall timeout
  }
  return true;
  ```
  For a fully-synchronous turn (no Executor → the LLM ran inline during `pump()`), `done()` is already
  true after the first `pump()` and `run_until` returns immediately — a strict superset of
  "pump-to-quiescence". `done` is a caller predicate (e.g. "reply captured or confirm pending").

**Header change:** add `#include <mutex>`/`<condition_variable>`/`<functional>`; the mutex+cv live in
`Impl`. The dtor must not deadlock (no handler runs in the dtor).

### 2. `Executor` worker pool — `include/hades/executor.h`, `src/core/executor.cpp`

```cpp
class Executor {
public:
  explicit Executor(unsigned threads);          // spawn N worker threads (default: a small fixed count)
  ~Executor();                                   // signal stop + join all workers (drains nothing in-flight beyond running tasks)
  Executor(const Executor&) = delete; Executor& operator=(const Executor&) = delete;
  void submit(std::function<void()> task);       // enqueue; some worker runs it
};
```

Workers pull `std::function<void()>` tasks off a mutex+cv task queue and run them. The Executor knows
nothing about the Blackboard — a submitted task closure captures whatever it needs (e.g. the provider +
the request + `bb`) and calls `bb.post(...)` itself (now thread-safe). Clean shutdown joins all workers.

### 3. LLMModule offload (the proof) — `include/hades/module/llm_module.h`, `src/module/llm_module.cpp`

- Add `void set_executor(Executor* e)` (non-owning; the Executor outlives the module). Default null.
- In the `LLM_REQUEST` handler:
  - **executor set (live):** `executor_->submit([this, req]{ LlmResponse r = provider_->complete(req); /* post LLM_RESPONSE + BUDGET_SPENT_USD via bb_ */ });` — the blocking HTTP runs on a worker; the bus
    is free; the worker posts the result back; `run_until` on the pump thread wakes and pumps it.
  - **executor null (tests):** run inline exactly as today (synchronous `complete()` + post).
- **Concurrency note:** `spent_` (the budget accumulator) is mutated in the worker. v1 has at most one
  in-flight LLM call per agent (turns are serial), so no data race; if multiple overlapping LLM calls are
  introduced later, `spent_` needs an atomic or a post-a-delta scheme. Document this. `provider_->complete()`
  must be reentrant for true concurrency — cpr is (per-call state); injected test providers run inline.

### 4. Live wiring — `app/agent_wiring.{h,cpp}`, `app/hades_main.cpp`, the front-end run loops

- The `Agent` owns an `Executor` (one per agent, lives for the session). The Manifest path creates it and
  calls `llm->set_executor(&executor)`. (Test path leaves the executor unset → inline LLM → tests green.)
- **HTTP `collect_()`** (`http_server_module.cpp`): replace `pump()` with
  `bb_->run_until([&]{ return got_reply_ || !pending_confirm_.is_null(); }, kTimeout)`. (Socket-free tests
  with no executor → completes immediately, unchanged.)
- **REPL `run_repl`/`run_repl_readline`** (`chat_module.cpp`): after posting `USER_MESSAGE`, replace
  `pump()` with `run_until(turn_done, kTimeout)`. Add a `turn_done_` flag set in the ChatModule's
  `ASSISTANT_MESSAGE` handler (the final answer / `[blocked]`/`[declined]`/`[stopped]` all arrive as
  `ASSISTANT_MESSAGE`) and reset before each user turn. The `CONFIRM_REQUEST` handler still reads y/N from
  stdin inline (during a `run_until` pump) and posts `CONFIRM_RESPONSE`; the turn continues until
  `ASSISTANT_MESSAGE` → `turn_done_`. (For the echo test with an inline handler, `turn_done_` is set during
  the first pump → `run_until` returns immediately.)

## Data flow (live, with offload)

```
USER_MESSAGE ─pump→ Arbiter.start_turn() posts LLM_REQUEST
                      └─ LLMModule: executor.submit(λ)        ── pump thread returns; bus free ──┐
                                                                                                  │ worker thread
   run_until(turn_done): pump (nothing) → cv.wait ………………………………………… provider.complete() (blocking HTTP)
                      ┌──────────────── cv.notify ── bb.post(LLM_RESPONSE) ◄───────────────────────┘
   wakes → pump → Arbiter handles LLM_RESPONSE → tool or ASSISTANT_MESSAGE → turn_done_ → run_until returns
```

## Error handling / safety

- A worker task that throws must not crash the process — the Executor wraps each task in try/catch (a
  failed LLM call posts an error `LLM_RESPONSE`/`ASSISTANT_MESSAGE` or is logged; never an unhandled throw
  on a worker thread = `std::terminate`).
- `run_until` returns `false` on overall timeout (a hung worker) — the front-end reports "[timed out]"
  rather than hanging forever.
- Executor shutdown joins workers; the `Agent`/`Executor` must outlive any in-flight task that captures
  `bb` — `Executor` is destroyed (joined) before the `Blackboard` in teardown order.
- No handler ever runs on a worker thread (invariant). `post()` is the only cross-thread entry point.

## Testing (TDD, GoogleTest)

- `test_blackboard` (extend) — a worker `std::thread` calls `post()` while the main thread is in
  `run_until`; the posted entry is delivered (handler runs on the main thread); `run_until` times out and
  returns false when the predicate never holds; existing `pump()` tests unchanged.
- `test_executor` (new) — `submit` runs a task (off the calling thread; verify via an atomic + join); many
  tasks all run; a throwing task doesn't crash; clean shutdown.
- `test_llm_module` (extend) — with an `Executor` + a provider that blocks briefly, `post(LLM_REQUEST)`
  returns before the response exists (bus not blocked), and `run_until(got_response)` collects it; without
  an executor, the existing inline path still works.
- Existing `test_arbiter`/`test_serve`/`test_chat`/e2e — green unchanged (no executor → inline → `pump`
  and `run_until` both drain synchronously).

## Out of scope (later, built ON this groundwork)

- **SSE/WebSocket streaming** (partial-token posts from the provider; an SSE endpoint).
- **Tool-call offload** (run_subprocess on a worker — same pattern as the LLM).
- **Concurrent turns / multiple in-flight LLM calls** (needs `spent_`/history concurrency hardening).
- The agent↔agent **Bridge** (cross-Blackboard forwarding; its own design).

## MOOS-IvP mapping

Keeps the single-threaded MOOSDB-style bus (deterministic, the asset) while reintroducing
process/thread isolation **only where blocking I/O lives** — same surgical philosophy as the
subprocess-isolated tools. The `Executor` is the off-thread analog of a tool subprocess: a place blocking
work happens without stalling the community's bus.
