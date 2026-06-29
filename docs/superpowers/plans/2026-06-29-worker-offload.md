# Worker-offload bus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. **Implementers run on OPUS** (per `feedback_sdd_implementer_opus`). Steps use checkbox (`- [ ]`) syntax. This is a delicate concurrency change — read the current files fully before editing each task.

**Goal:** Keep the single-threaded deterministic bus, but offload blocking work (the LLM HTTP call) to a worker thread that posts its result back. Add a thread-safe `post()`, a `run_until()` event loop, and an `Executor`. Offload is opt-in (Executor set) so all existing tests stay synchronous + green.

**Architecture:** Subscriber handlers run ONLY on the pump thread (the determinism invariant). Worker threads only call `post()` (thread-safe enqueue). `run_until(pred)` pumps + sleeps on a condvar until a worker posts, then pumps again. The LLMModule offloads `complete()` to an `Executor` when one is set; otherwise it runs inline (today's path).

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · `std::thread`/`std::mutex`/`std::condition_variable` · GoogleTest. Design spec: `docs/superpowers/specs/2026-06-29-worker-offload-design.md` (read it first).

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`**.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **HARD INVARIANT:** subscriber handlers run ONLY on the pump thread. Worker threads call `post()` only. Never dispatch from a worker.
- **Offload is opt-in:** active only when an `Executor` is set on the LLMModule. With no executor, behavior is byte-identical to today (inline). All 119 existing tests must stay green.
- **No unhandled throw on a worker thread** (= `std::terminate`): the Executor wraps each task in try/catch.
- **Teardown order:** the `Executor` must be joined/destroyed before the `Blackboard` (in-flight tasks capture `bb`).
- Run sanitizers where practical: `nix develop --command cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=thread"` for a TSan pass on the new concurrency (optional but recommended in T1/T2 review).

---

## File Structure

```
include/hades/blackboard.h        T1 (modify)  mutex/cv in Impl; run_until()
src/core/blackboard.cpp           T1 (modify)  thread-safe post/pump/get + run_until
include/hades/executor.h          T2 (new)
src/core/executor.cpp             T2 (new)
include/hades/module/llm_module.h T3 (modify)  set_executor()
src/module/llm_module.cpp         T3 (modify)  offload when executor set
app/agent_wiring.{h,cpp}          T4 (modify)  Agent owns Executor; wire to LLMModule
app/hades_main.cpp                T4 (modify)  (teardown order already bb-before-agent? verify)
src/module/chat_module.cpp        T4 (modify)  run_until + turn_done_
src/module/http_server_module.cpp T4 (modify)  collect_ uses run_until
tests/test_blackboard.cpp         T1 (extend)
tests/test_executor.cpp           T2 (new)
tests/test_llm_module.cpp         T3 (extend)
```

---

## Task 1: Thread-safe Blackboard + `run_until`

**Files:** Modify `include/hades/blackboard.h`, `src/core/blackboard.cpp`, `tests/test_blackboard.cpp`. **Read both current files fully first** (the `Impl` struct, `post`/`pump`/`get`/`now`/`queued`).

**Interfaces — Produces:** thread-safe `post()` (callable from any thread); `bool run_until(const std::function<bool()>& done, double timeout_s)`. `pump()`/`get()`/`subscribe()` semantics unchanged for single-thread callers.

- [ ] **Step 1: Failing tests** — append to `tests/test_blackboard.cpp`:

```cpp
#include <atomic>
#include <chrono>
#include <thread>

TEST(Blackboard, RunUntilDeliversAWorkerThreadPost) {
  Blackboard bb;
  std::atomic<int> got{0};
  bb.subscribe("FROM_WORKER", [&](const Entry&){ got.fetch_add(1); });
  std::thread worker([&]{
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bb.post("FROM_WORKER", 1, "worker");   // posted from another thread
  });
  bool ok = bb.run_until([&]{ return got.load() > 0; }, 5.0);
  worker.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(got.load(), 1);   // handler ran on the main (run_until) thread
}

TEST(Blackboard, RunUntilTimesOutWhenPredicateNeverHolds) {
  Blackboard bb;
  bool ok = bb.run_until([]{ return false; }, 0.2);   // ~200ms
  EXPECT_FALSE(ok);
}

TEST(Blackboard, RunUntilReturnsImmediatelyOnInlineCompletion) {
  Blackboard bb;
  bool fired = false;
  bb.subscribe("A", [&](const Entry&){ fired = true; });
  bb.post("A", 1, "s");                       // already queued, inline handler
  bool ok = bb.run_until([&]{ return fired; }, 5.0);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(fired);
}
```

- [ ] **Step 2: Run, expect FAIL** — `run_until` undeclared.

- [ ] **Step 3: Implement.**

`include/hades/blackboard.h`: add `#include <condition_variable>`, `#include <functional>`, `#include <mutex>`; declare `bool run_until(const std::function<bool()>& done, double timeout_s);` in the public API. (The mutex + cv live in `Impl` in the .cpp.)

`src/core/blackboard.cpp`: add `std::mutex mu;` and `std::condition_variable cv;` to `Impl`. Then:
- `post()`: `{ std::lock_guard lk(p_->mu); /* seq++, latest[key]=e, log->append, queue.push_back */ } p_->cv.notify_one();`
- `pump()`: loop — `std::unique_lock lk(p_->mu); if(queue.empty()) break; Entry e = queue.front(); queue.pop_front(); lk.unlock(); /* dispatch e to matching subs (min_interval logic unchanged) — handlers run UNLOCKED */`.
- `get()`: lock `mu` around the `latest` read.
- `now()`/`queued()`: `queued()` locks; `now()` unchanged (monotonic clock).
- `run_until`:
```cpp
bool Blackboard::run_until(const std::function<bool()>& done, double timeout_s) {
  const double deadline = now() + timeout_s;
  while (!done()) {
    pump();
    if (done()) return true;
    std::unique_lock<std::mutex> lk(p_->mu);
    if (p_->queue.empty())
      p_->cv.wait_for(lk, std::chrono::milliseconds(20), [&]{ return !p_->queue.empty(); });
    lk.unlock();
    if (now() >= deadline) return false;
  }
  return true;
}
```
**Care:** never hold `mu` while calling a handler or `done()`. `pump()` must release the lock before dispatch (subscriber `post()` re-locks). Verify no recursive-lock/deadlock.

- [ ] **Step 4: Build + test** — `-R Blackboard`, PASS. Then full suite green. (Recommended: a TSan build pass to check the worker-post test is race-free.)
- [ ] **Step 5: Commit** — `feat: thread-safe Blackboard post/pump + run_until event loop`

---

## Task 2: `Executor` worker pool

**Files:** Create `include/hades/executor.h`, `src/core/executor.cpp`, `tests/test_executor.cpp`. Modify `CMakeLists.txt` (`target_sources(hades_core PRIVATE src/core/executor.cpp)` + test source).

**Interfaces — Produces:** `Executor(unsigned threads)`, `submit(std::function<void()>)`, joining dtor. Move-/copy-deleted.

- [ ] **Step 1: Failing tests** — `tests/test_executor.cpp`:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "hades/executor.h"
using namespace hades;

TEST(Executor, RunsSubmittedTaskOffThread) {
  std::atomic<int> n{0};
  std::thread::id caller = std::this_thread::get_id();
  std::atomic<std::thread::id> ran_on{};
  { Executor ex(2);
    ex.submit([&]{ ran_on = std::this_thread::get_id(); n.fetch_add(1); });
    // dtor joins -> task definitely completed
  }
  EXPECT_EQ(n.load(), 1);
  EXPECT_NE(ran_on.load(), caller);   // ran on a worker, not the caller
}

TEST(Executor, RunsManyTasks) {
  std::atomic<int> n{0};
  { Executor ex(4);
    for (int i = 0; i < 100; ++i) ex.submit([&]{ n.fetch_add(1); });
  }
  EXPECT_EQ(n.load(), 100);
}

TEST(Executor, ThrowingTaskDoesNotCrash) {
  std::atomic<int> n{0};
  { Executor ex(2);
    ex.submit([]{ throw std::runtime_error("boom"); });
    ex.submit([&]{ n.fetch_add(1); });
  }
  EXPECT_EQ(n.load(), 1);   // the throwing task was contained; the other still ran
}
```

- [ ] **Step 2: Run, expect FAIL** (no executor.h).
- [ ] **Step 3: Implement** — a mutex+cv task queue, N worker threads looping `wait → pop → run-in-try/catch`; dtor sets a stop flag, notifies all, joins. Each worker wraps the task in `try { task(); } catch(...) {}` (no unhandled throw). `submit` locks, pushes, `notify_one`.
- [ ] **Step 4: Build + test** — `-R Executor` PASS; full suite green. (Recommended: TSan pass.)
- [ ] **Step 5: Commit** — `feat: Executor worker pool (offload blocking work off the bus thread)`

---

## Task 3: LLMModule offload (opt-in)

**Files:** Modify `include/hades/module/llm_module.h`, `src/module/llm_module.cpp`, `tests/test_llm_module.cpp`. **Read the current llm_module first** (the `LLM_REQUEST` handler that calls `provider_->complete()` + posts `LLM_RESPONSE`/`BUDGET_SPENT_USD`).

**Interfaces — Produces:** `void set_executor(Executor*)`. With an executor set, `complete()` runs on a worker that posts the result back; with none, inline as today.

- [ ] **Step 1: Failing test** — append to `tests/test_llm_module.cpp`:

```cpp
TEST(LLMModule, OffloadsToExecutorWithoutBlockingTheBus) {
  // A provider whose complete() blocks ~50ms; with an Executor the post(LLM_REQUEST) must
  // return before the response exists, and run_until collects it.
  struct SlowProvider : Provider {
    LlmResponse complete(const LlmRequest&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      LlmResponse r; r.text = "slow ok"; return r;
    }
  };
  Blackboard bb;
  Executor ex(2);
  LLMModule m(std::make_unique<SlowProvider>());
  m.set_executor(&ex);
  Block cfg; cfg.kv["price_per_mtok"] = "0";
  m.on_start(cfg, bb); m.on_attach(bb);
  std::string text;
  bb.subscribe("LLM_RESPONSE", [&](const Entry& e){ text = e.value.value("text", std::string{}); });
  bb.post("LLM_REQUEST", {{"messages", nlohmann::json::array()}, {"tools", nlohmann::json::array()}}, "arb");
  // Right after pump returns, the worker is still sleeping -> no response yet (bus not blocked).
  bb.pump();
  EXPECT_TRUE(text.empty());                          // offloaded: not done synchronously
  bool ok = bb.run_until([&]{ return !text.empty(); }, 5.0);
  EXPECT_TRUE(ok);
  EXPECT_EQ(text, "slow ok");
}
```
(Add `#include <thread>`/`#include <chrono>`/`#include "hades/executor.h"` to the test.)

- [ ] **Step 2: Run, expect FAIL** — `set_executor` undeclared (and without offload, the response would already be present after `pump()`, failing `EXPECT_TRUE(text.empty())`).
- [ ] **Step 3: Implement** — add `Executor* executor_ = nullptr;` + `set_executor`. In the `LLM_REQUEST` handler, factor the "call complete() + post LLM_RESPONSE + post BUDGET_SPENT_USD" into a lambda `run(req)`; if `executor_`, `executor_->submit([this,req]{ run(req); })`; else `run(req)` inline. Keep `spent_` mutation inside `run` (document: single in-flight call per agent → no race). Existing tests (no executor) hit the inline branch unchanged.
- [ ] **Step 4: Build + test** — `-R LLMModule` PASS (new + existing); full suite green.
- [ ] **Step 5: Commit** — `feat: LLMModule offloads the LLM call to an Executor when set (inline otherwise)`

---

## Task 4: Live wiring — Agent owns an Executor; front-ends use `run_until`

**Files:** Modify `app/agent_wiring.{h,cpp}`, `app/hades_main.cpp`, `src/module/chat_module.cpp`, `src/module/http_server_module.cpp`. **Read each current file first.**

**Interfaces:** the live agent offloads the LLM; REPL + HTTP drive turns via `run_until(turn_done, timeout)`. Synchronous behavior preserved (no executor in tests → inline → both `pump` and `run_until` drain at once).

- [ ] **Step 1: Failing test** — there is no clean new unit (this is live integration). The "failing" state: add the `Executor` member to `Agent` and wire it; the existing suite must stay green. Optionally add an integration test in `tests/test_pantler_wiring.cpp` (or a new `tests/test_offload_e2e.cpp`) using the existing `take`/build path with a `SlowProvider`-style injected provider + executor that drives a full turn via `run_until`. Keep it cheap and offline.

- [ ] **Step 2–3: Implement.**
  - **`Agent`** (`agent_wiring.h`): add `std::unique_ptr<Executor> executor;` — declared so it is destroyed in an order that joins workers BEFORE the modules/blackboard tear down (verify destruction order in `hades_main`: `Blackboard bb` is declared before `Agent agent`, so `agent` (and its executor) destruct first → workers join before `bb` dies — correct). Include `hades/executor.h`.
  - **Manifest path** (`build_agent(bb,m)`): create `a.executor = std::make_unique<Executor>(N)` and, if `a.llm`, `a.llm->set_executor(a.executor.get())`. (Test overload leaves `executor` null → inline LLM → tests green.)
  - **`http_server_module.cpp` `collect_()`**: replace `bb_->pump();` with `bb_->run_until([this]{ return got_reply_ || !pending_confirm_.is_null(); }, kCollectTimeoutS);` (define a generous timeout, e.g. 180s). Socket-free tests (no executor) complete immediately. If `run_until` times out, return `{{"reply","[timed out]"}}`.
  - **`chat_module.cpp`**: add `bool turn_done_ = false;`. Set it `true` in the `ASSISTANT_MESSAGE` handler. In `run_repl`/`run_repl_readline`, before posting a `USER_MESSAGE` set `turn_done_ = false`, then after posting call `bb_->run_until([this]{ return turn_done_; }, kTurnTimeoutS);` instead of `pump()`. The `CONFIRM_REQUEST` handler still reads y/N inline + posts `CONFIRM_RESPONSE` (it runs during a `run_until` pump; the turn continues to `ASSISTANT_MESSAGE`). Existing `test_chat` (inline echo handler) sets `turn_done_` during the first pump → `run_until` returns at once.
  - **`hades_main.cpp`**: no logic change beyond confirming teardown order (bb before agent). The serve/REPL dispatch already calls `agent.serve->listen` / `agent.chat->run_repl`, which now use `run_until` internally.

- [ ] **Step 4: Build + FULL suite** — all 119 + any new green. Existing `test_serve`/`test_chat`/e2e use no executor → inline → unchanged. Run a TSan build of the suite if practical.
- [ ] **Step 5: Manual smoke (needs key)** — `./build/hades manifests/dev.hades` chats (LLM now offloaded; the REPL prints the prompt and the turn completes via run_until); `--serve` works; responsiveness unchanged or better.
- [ ] **Step 6: Commit** — `feat: live agent offloads the LLM via an Executor; REPL/HTTP drive turns with run_until`

---

## Self-Review (against the spec)

- **Spec coverage:** thread-safe post/pump + run_until (T1); Executor (T2); opt-in LLM offload (T3); live wiring + front-end run_until + turn_done (T4). Invariant (handlers on pump thread only; workers post() only) preserved. Offload opt-in → 119 tests stay green.
- **Out of scope honored:** no SSE, no tool offload, no concurrent turns, no Bridge.
- **Risk notes for the builder:** the deadlock surface is `pump()`/`run_until` holding `mu` across a handler — they must NOT. The teardown order (Executor joined before Blackboard) is load-bearing. `spent_` is single-in-flight-safe only.

## Verification

1. Full suite green (offline; no executor in tests). 2. TSan pass on the new concurrency (T1/T2 + the offload test). 3. Manual: live chat + `--serve` with the LLM offloaded; the bus stays responsive; no hang/deadlock; clean exit (workers join).
