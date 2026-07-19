# Tool-offload + Background Tool Execution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tools run off the pump thread (bus stays live during a long tool), and the LLM can opt any call into background (`background: true` → immediate `{started, task_id}`, result delivered cross-turn via a system-message fold).

**Architecture:** Phase 1 mirrors the LLM-offload pattern in ToolRunner (pump-thread resolve → value-capture worker → post back) and closes the documented `arbiter.cpp` deferral: epoch on `TOOL_REQUEST`/`TOOL_RESULT` + abandoned-turn orphan pop. Phase 2 adds a background registry inside ToolRunner (`BG_DONE` worker event → pump-thread ring → `BG_TASKS` latest-value block) folded by the Arbiter like SKILLS_ANNOUNCE.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, `hades::Executor` worker pool, `hades::Blackboard` (`post` thread-safe, handlers pump-thread-only, `run_until`).

**Spec:** `docs/superpowers/specs/2026-07-19-tool-offload-design.md` (approved, committed `3ceca5c` on branch `feat/tool-offload`).

## Global Constraints

- **Every build/test command runs inside `nix develop`.** ASan lane: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **752/752 green** before Task 1. NEVER reconfigure `build/` without `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` (flags live only in the CMake cache).
- TSan lane exists at `build-tsan/`; the final task runs it too.
- Branch `feat/tool-offload`. Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- Never stage: `manifests/dev.local.hades`, `manifests/pi.hades`, `manifests/dev2.hades`, `memory/`, `skills/greek-greeting/`, `skills/ponytail/`, `build*/`.
- Pump-thread handlers never throw; workers capture ONLY value copies + non-owning `Blackboard*` (LLMModule precedent) and post back — they read no module field.
- Exact values (from the spec, verbatim): bus keys `DROPPED_STALE_TOOL_RESULT`, `BG_DONE`, `BG_TASKS`; block header `Background tasks (started earlier with background:true):`; finished ring cap **5**; output truncation **2000** bytes (UTF-8-boundary walk-back + `" (truncated)"` marker); `max_background` default **4** (bounds 1..64); `background_timeout_s` default **600**; effective bg timeout `max(per-tool timeout_s, background_timeout_s)`; `kExecutorThreads` **2 → 8**; runner default timeout key `Tools.timeout_s` (default 30, legacy key `timeout` still honored).
- Epoch gate on `TOOL_RESULT` is **tolerant-if-absent**: a result with NO `epoch` field is never dropped (hand-posted/legacy compat — many existing tests post `TOOL_RESULT` without epoch); a PRESENT mismatched epoch is dropped.
- `background` is **harness-owned**: stripped from args before the subprocess/MCP call in ALL paths (foreground too); non-bool value = absent (house rule).
- The empty-manifest/test path must stay byte-identical: no executor on ToolRunner → inline execution → the existing suite passes unchanged.

---

## File Structure

```
include/hades/arbiter.h                 (unchanged — all Task 1/5 edits are .cpp-local)
src/apps/arbiter/arbiter.cpp            T1 epoch stamp/gate + orphan pop; T5 BG_TASKS fold
include/hades/module/tool_runner.h      T2 set_executor; T4 bg registry members + trunc_utf8_bytes
src/apps/tool_runner/tool_runner.cpp    T2 offload restructure; T4 background branch + BG_DONE/BG_TASKS + schema append
app/agent_wiring.cpp                    T3 Tools block, extended invariant, set_executor, threads 8
tests/test_arbiter.cpp                  T1 (+5 tests), T5 (+2 tests)
tests/test_tool_offload.cpp             T2 (new, 3 tests), T4 (+6 tests)
tests/test_tools_wiring.cpp             T3 (new, 3 tests)
CMakeLists.txt                          T2, T3 (register the two new test files)
prompts/soul.md                         T5 background guidance section
docs/manifest-reference.md              T5 Tools block section + notes
CLAUDE.md                               T5 feature record
manifests/dev.hades                     T5 commented Tools block
```

---

## Task 1: Epoch on TOOL_RESULT + abandoned-turn orphan pop (Arbiter)

**Files:**
- Modify: `src/apps/arbiter/arbiter.cpp` (TURN_ABANDONED handler ~line 99; `dispatch_or_gate` TOOL_REQUEST post ~line 393; `on_confirm` TOOL_REQUEST post ~line 443; `on_tool_result` ~line 410)
- Test: `tests/test_arbiter.cpp` (append)

**Interfaces:**
- Consumes: existing `turn_epoch_` (`std::uint64_t`, private member), `clear_pending()`, `history_size()` (public, test observability), `file_versions_`/`pending_file_ops_` staleness maps.
- Produces: every `TOOL_REQUEST` carries `{"epoch", turn_epoch_}`; `on_tool_result` drops a present-but-mismatched epoch posting `DROPPED_STALE_TOOL_RESULT {epoch, current}` (AFTER version harvest, BEFORE history append); `TURN_ABANDONED` pops trailing `assistant(tool_calls)` from `history_`. Task 2's ToolRunner echoes the epoch.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (match file style; all includes already present):

```cpp
TEST(Arbiter, ToolRequestCarriesTurnEpoch) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json toolreq;
  bb.subscribe("TOOL_REQUEST",[&](const Entry& e){ toolreq=e.value; });
  bb.post("USER_MESSAGE","go","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","x"},
          {"arguments",nlohmann::json::object()}}}}, "llm"); bb.pump();
  EXPECT_EQ(toolreq.value("epoch", 0), 1);
}
TEST(Arbiter, StaleEpochToolResultIsDroppedNotContinued) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  nlohmann::json dropped;
  bb.subscribe("DROPPED_STALE_TOOL_RESULT",[&](const Entry& e){ dropped=e.value; });
  bb.post("USER_MESSAGE","go","chat"); bb.pump();          // epoch 1, first LLM_REQUEST
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","x"},
          {"arguments",nlohmann::json::object()}}}}, "llm"); bb.pump();
  bb.post("TURN_ABANDONED", true, "chat"); bb.pump();      // epoch -> 2 (+ orphan popped)
  const std::size_t before = a.history_size();
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"x",1}}},{"epoch",1}},
          "tool_runner"); bb.pump();
  EXPECT_FALSE(dropped.is_null());
  EXPECT_EQ(reqs.size(), 1u);                    // no continuation request fired
  EXPECT_EQ(a.history_size(), before);           // no orphan tool message appended
}
TEST(Arbiter, ToolResultWithoutEpochStillContinues) {   // legacy/hand-posted compat
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("USER_MESSAGE","go","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","x"},
          {"arguments",nlohmann::json::object()}}}}, "llm"); bb.pump();
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",nlohmann::json::object()}},
          "tool_runner"); bb.pump();
  EXPECT_EQ(reqs.size(), 2u);                    // continuation fired (no epoch = not gated)
}
TEST(Arbiter, TurnAbandonedPopsTrailingAssistantToolCalls) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","go","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","x"},
          {"arguments",nlohmann::json::object()}}}}, "llm"); bb.pump();
  EXPECT_EQ(a.history_size(), 2u);               // user + assistant(tool_calls)
  bb.post("TURN_ABANDONED", true, "chat"); bb.pump();
  EXPECT_EQ(a.history_size(), 1u);               // orphan pair-head popped
  bb.post("USER_MESSAGE","again","chat"); bb.pump();
  for (const auto& msg : req["messages"])        // next request has no dangling tool_calls
    EXPECT_FALSE(msg.contains("tool_calls"));
}
TEST(Arbiter, StaleSuccessfulWriteStillHarvestsVersion) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json toolreq;
  bb.subscribe("TOOL_REQUEST",[&](const Entry& e){ toolreq=e.value; });
  bb.post("USER_MESSAGE","go","chat"); bb.pump();          // epoch 1
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","write_file"},
          {"arguments",{{"path","w.txt"},{"content","x"}}}}}}, "llm"); bb.pump();
  bb.post("TURN_ABANDONED", true, "chat"); bb.pump();      // epoch -> 2
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"version","abc123"}}},
          {"epoch",1}}, "tool_runner"); bb.pump();         // stale: dropped BUT harvested
  bb.post("USER_MESSAGE","edit","chat"); bb.pump();        // epoch 3
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",3},{"tool_call",{{"id","c2"},{"name","edit_file"},
          {"arguments",{{"path","w.txt"},{"old_string","a"},{"new_string","b"}}}}}}, "llm"); bb.pump();
  EXPECT_EQ(toolreq["args"].value("expect_version",""), "abc123");   // disk truth injected
}
```

- [ ] **Step 2: Run to verify they fail.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Arbiter`
Expected: the 5 new tests FAIL (`epoch` missing from TOOL_REQUEST; stale result continues; history_size 2 after abandonment; no expect_version).

- [ ] **Step 3: Implement.** Four edits in `src/apps/arbiter/arbiter.cpp`:

**(a)** `dispatch_or_gate` — the direct TOOL_REQUEST post becomes:

```cpp
    bb_->post("TOOL_REQUEST",
              {{"id", act.tool_id}, {"tool", act.tool}, {"args", act.args},
               {"epoch", turn_epoch_}},
              "arbiter");
```

**(b)** `on_confirm` — the approved-path TOOL_REQUEST post becomes:

```cpp
    bb_->post("TOOL_REQUEST",
              {{"id", pending_.value("tool_id", "")},
               {"tool", pending_.value("tool", "")},
               {"args", pending_.contains("args") ? pending_["args"] : nlohmann::json::object()},
               {"epoch", turn_epoch_}},
              "arbiter");
```

**(c)** `on_tool_result` — insert AFTER the `pending_file_ops_` harvest block and BEFORE the `append_history` call:

```cpp
  // Freshness gate (tool-offload): a result stamped with a superseded epoch — its turn was
  // abandoned or rotated while the offloaded worker ran — must not continue the CURRENT
  // turn. The version harvest above already ran: a stale-but-successful write DID change
  // the disk, so its version is truth. A result with NO epoch field is NOT gated
  // (hand-posted/legacy producers).
  if (v.contains("epoch") &&
      (v["epoch"].is_number_integer() || v["epoch"].is_number_unsigned()) &&
      v["epoch"].get<std::uint64_t>() != turn_epoch_) {
    bb_->post("DROPPED_STALE_TOOL_RESULT",
              {{"epoch", v["epoch"]}, {"current", turn_epoch_}}, "arbiter");
    return;
  }
```

**(d)** the `TURN_ABANDONED` subscriber body gains the orphan pop (and update the stale
`NOTE: tools run SYNCHRONOUSLY today` comment above it — tools are offloaded now, the
epoch extension it deferred is THIS change):

```cpp
  bb.subscribe("TURN_ABANDONED", [this](const Entry&) {
    ++turn_epoch_;
    clear_pending();
    // Tool-offload: an abandoned turn can leave a trailing assistant(tool_calls) whose
    // TOOL_RESULT is dropped by the epoch gate above (or never arrives). A later request
    // would then carry the orphan pair-head — provider-invalid. Pop it (the same sanitize
    // load_history applies on resume; the on-disk session line stays, resume handles it).
    while (!history_.empty() &&
           history_.back().value("role", "") == "assistant" &&
           history_.back().contains("tool_calls"))
      history_.pop_back();
  });
```

- [ ] **Step 4: Build + test.** Same commands. Expected: 5 new tests PASS, ALL existing Arbiter tests still pass (they post TOOL_RESULT without epoch → tolerant path). Then full suite: 757/757.
- [ ] **Step 5: Commit.**

```bash
git add src/apps/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: epoch-stamp TOOL_REQUEST/RESULT + abandoned-turn orphan pop (tool-offload phase 1a)"
```

---

## Task 2: ToolRunner Executor offload

**Files:**
- Modify: `include/hades/module/tool_runner.h`, `src/apps/tool_runner/tool_runner.cpp` (the `TOOL_REQUEST` handler, ~lines 44-88)
- Create: `tests/test_tool_offload.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hades::Executor::submit(std::function<void()>)` (`include/hades/executor.h`); `Blackboard::post` (thread-safe) and `run_until`; `run_subprocess`/`mcp_call` (already thread-safe per-call — they touch only their arguments).
- Produces: `ToolRunner::set_executor(Executor*)` (null default → inline, byte-identical); a file-local `execute_tool(const ToolEntry&, name, real, args, timeout) -> std::pair<bool, nlohmann::json>` execution core that Task 4's background worker reuses; `TOOL_RESULT` echoes the request's `epoch` when present (absent in → absent out).

- [ ] **Step 1: Write the failing tests** — create `tests/test_tool_offload.cpp`:

```cpp
// tests/test_tool_offload.cpp — ToolRunner Executor offload: the bus stays live mid-tool
//
// Writes a self-contained slow native tool (shell script) at runtime — no CMake fixture —
// and proves: (1) an offloaded tool completes a request via run_until; (2) the bus
// dispatches OTHER traffic while the tool is still running; (3) the request's epoch is
// echoed into TOOL_RESULT; (4) no executor -> inline synchronous (legacy shape, no epoch
// key when the request had none). The Executor is declared AFTER the modules in each test
// -> destroyed FIRST -> joins its worker while ToolRunner + Blackboard are alive (the
// load-bearing live teardown order; offload_e2e precedent).
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/executor.h"
#include "hades/module/tool_runner.h"
using namespace hades;

namespace {
// A native tool answering describe immediately and sleeping before answering a call.
std::string write_slow_tool(const char* tag, double sleep_s) {
  const std::string p = ::testing::TempDir() + "/slow_tool_" + tag + "_" +
                        std::to_string(::getpid()) + ".sh";
  std::ofstream f(p);
  f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
    << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"slow\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
    << "*) sleep " << sleep_s << "\n"
    << "   echo '{\"ok\":true,\"result\":{\"note\":\"slow done\"}}' ;;\nesac\n";
  f.close();
  ::chmod(p.c_str(), 0755);
  return p;
}
}  // namespace

TEST(ToolOffload, OffloadedToolCompletesViaRunUntilAndEchoesEpoch) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("a", 0.2);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  Executor ex(2);                        // declared last: destroyed first, joins the worker
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}, {"epoch", 7}},
          "arbiter");
  ASSERT_TRUE(bb.run_until([&] { return !result.is_null(); }, 5.0));
  EXPECT_TRUE(result.value("ok", false));
  EXPECT_EQ(result["content"].value("note", ""), "slow done");
  EXPECT_EQ(result.value("epoch", 0), 7);          // echoed through the worker
}

TEST(ToolOffload, BusDispatchesOtherTrafficWhileToolRuns) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("b", 0.5);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  bool side = false;
  nlohmann::json result;
  bb.subscribe("SIDE_CHANNEL", [&](const Entry&) { side = true; });
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}}, "arbiter");
  bb.post("SIDE_CHANNEL", true, "test");
  // The side post dispatches on the FIRST pump inside run_until — long before the 0.5s
  // tool finishes. Pre-offload, the inline handler would have blocked pump() until done.
  ASSERT_TRUE(bb.run_until([&] { return side; }, 5.0));
  EXPECT_TRUE(result.is_null());                   // tool still running (0.5s margin)
  ASSERT_TRUE(bb.run_until([&] { return !result.is_null(); }, 5.0));
  EXPECT_TRUE(result.value("ok", false));
}

TEST(ToolOffload, NoExecutorRunsInlineSynchronously) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("c", 0.0);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}}, "arbiter");
  bb.pump();                                       // no executor: ONE pump completes it
  EXPECT_TRUE(result.value("ok", false));
  EXPECT_FALSE(result.contains("epoch"));          // absent in -> absent out (legacy shape)
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt`, next to the other `target_sources(hades_tests ...)` test lines, add:

```cmake
target_sources(hades_tests PRIVATE tests/test_tool_offload.cpp)
```

Run the build. Expected: compile FAIL — `ToolRunner` has no member `set_executor`.

- [ ] **Step 3: Implement.**

`include/hades/module/tool_runner.h` — add a forward declaration and the seam (member next to `bb_`):

```cpp
class Blackboard;
class Executor;
```

```cpp
  // Opt-in offload (the LLMModule::set_executor pattern): when set, tool execution runs on
  // a worker and posts TOOL_RESULT back; null (default) -> inline on the pump thread, so
  // the test path is byte-identical. Non-owning: the Executor is joined before modules die.
  void set_executor(Executor* ex) { executor_ = ex; }
```

```cpp
  Executor* executor_ = nullptr;  // non-owning; see set_executor
```

`src/apps/tool_runner/tool_runner.cpp` — add `#include "hades/executor.h"` and `#include <cstdint>`, then add the execution core ABOVE `ToolRunner::on_attach` (file-local, in the same anonymous-free style as `split_ws`):

```cpp
// Pure execution core: run a RESOLVED tool (native subprocess or MCP call) and return
// {ok, content}. Thread-safe by construction — it reads ONLY its parameters (the ToolEntry
// is a value copy), so it behaves identically on the pump thread (inline) or on an
// Executor worker. All mutable-registry access (find_by_tool_name / mcp_real_name lazily
// warm caches) must happen BEFORE this, on the pump thread.
static std::pair<bool, nlohmann::json> execute_tool(const ToolEntry& te,
                                                    const std::string& name,
                                                    const std::string& real,
                                                    const nlohmann::json& args,
                                                    double timeout) {
  if (te.kind == "native") {
    nlohmann::json call{{"call", name}, {"args", args}};
    auto r = run_subprocess(split_ws(te.command), call.dump(), timeout);
    if (r.timed_out) return {false, {{"error", "tool timed out: " + name}}};
    auto j = nlohmann::json::parse(r.out, nullptr, false);  // guarded
    if (!j.is_object()) return {false, {{"error", "bad tool output"}}};
    return {j.value("ok", false),
            j.contains("result") ? j["result"] : nlohmann::json::object()};
  }
  nlohmann::json content = mcp_call(te, real.empty() ? name : real, args, timeout);
  return {content.is_object() && !content.contains("error"), content};
}
```

Replace the body of the `TOOL_REQUEST` subscriber in `ToolRunner::on_attach` with:

```cpp
  bb.subscribe("TOOL_REQUEST", [this](const Entry& e) {
    // Guard all external (blackboard) JSON access.
    std::string id, name;
    std::uint64_t epoch = 0;
    bool has_epoch = false;
    nlohmann::json args = nlohmann::json::object();
    if (e.value.is_object()) {
      id = e.value.value("id", "");
      name = e.value.value("tool", "");
      if (e.value.contains("args") && e.value["args"].is_object()) args = e.value["args"];
      if (e.value.contains("epoch") &&
          (e.value["epoch"].is_number_integer() || e.value["epoch"].is_number_unsigned())) {
        epoch = e.value["epoch"].get<std::uint64_t>();
        has_epoch = true;
      }
    }

    // PUMP THREAD ONLY: find_by_tool_name/mcp_real_name lazily warm mutable caches. The
    // worker below receives value copies exclusively.
    const ToolEntry* te = reg_.find_by_tool_name(name);
    if (!te) {
      nlohmann::json out{{"id", id}, {"ok", false},
                         {"content", {{"error", "unknown tool: " + name}}}};
      if (has_epoch) out["epoch"] = epoch;
      bb_->post("TOOL_RESULT", out, "tool_runner", id);
      return;
    }
    // Per-tool override (Tool block timeout_s); 0 -> the runner-wide default.
    const double timeout = te->timeout_s > 0.0 ? te->timeout_s : timeout_s_;
    const ToolEntry entry = *te;                    // value copy: worker independent of registry
    const std::string real = reg_.mcp_real_name(name);

    // CONCURRENCY (LLMModule precedent): the closure captures value copies + ONE non-owning
    // pointer (`bb2`); `this` is NOT captured, so the worker can reach no mutable module
    // field. Teardown order joins the Executor before modules/Blackboard die.
    Blackboard* bb2 = bb_;
    auto run = [bb2, id, name, args, entry, real, timeout, epoch, has_epoch] {
      auto [ok, content] = execute_tool(entry, name, real, args, timeout);
      nlohmann::json out{{"id", id}, {"ok", ok}, {"content", content}};
      if (has_epoch) out["epoch"] = epoch;          // echo the turn stamp (absent in -> absent out)
      bb2->post("TOOL_RESULT", out, "tool_runner", id);
    };
    if (executor_) executor_->submit(run);
    else run();                                     // inline (default, unchanged)
  });
```

Also update the file-header comment block (lines 1-6) to mention the offload: tools now run on the Executor when one is set (LLM-offload pattern), inline otherwise.

- [ ] **Step 4: Build + test.** `-R ToolOffload` → 3 pass; then FULL suite → 760/760 (existing tool_runner/e2e/mcp tests all exercise the inline path unchanged).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/tool_runner.h src/apps/tool_runner/tool_runner.cpp tests/test_tool_offload.cpp CMakeLists.txt
git commit -m "feat: ToolRunner Executor offload — tools off the pump thread (LLM-offload pattern)"
```

---

## Task 3: Wiring — Tools block, extended idle invariant, executor on ToolRunner

**Files:**
- Modify: `app/agent_wiring.cpp` (invariant block ~lines 636-658; `a.tools->on_start(Block{}, bb)` ~line 369; wire_agent signature + Manifest-overload call ~lines 609/735; executor block ~lines 728-731)
- Modify: `src/apps/tool_runner/tool_runner.cpp` (`on_start`: accept `timeout_s` key)
- Create: `tests/test_tools_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ToolRunner::set_executor` (Task 2); `set_pos_double_on_string`, `MalConfig`, `Manifest::of`.
- Produces: optional `Tools { timeout_s max_background background_timeout_s }` manifest block reaching `ToolRunner::on_start` (Task 4 reads the two background keys there); launch `MalConfig` when `turn_idle_timeout_s <= max(llm_timeout_s, Tools.timeout_s default 30, every Tool block's timeout_s)`; live path calls `a.tools->set_executor(a.executor.get())`; `kExecutorThreads` = 8.

- [ ] **Step 1: Write the failing tests** — create `tests/test_tools_wiring.cpp`:

```cpp
// tests/test_tools_wiring.cpp — manifest-path Tools block + the extended idle invariant
//
// With tools offloaded, a silent stretch can be a foreground tool run: boot must refuse a
// manifest whose idle ceiling doesn't outlast every foreground-effective tool timeout.
// Roster has NO llm/front-end (wiring-test precedent) — the invariant fires before any
// heavy build. Multi-line Tool blocks only (one-kv-per-line parser fails loud on packing).
#include <gtest/gtest.h>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
using namespace hades;

static Manifest mk(const std::string& extra) {
  return parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n" + extra);
}

TEST(ToolsWiring, PerToolTimeoutOverIdleCeilingThrows) {
  Blackboard bb;
  Manifest m = mk("Tool = big\n{\n  native = /bin/true\n  timeout_s = 1000\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);     // 1000 >= default idle 900
}
TEST(ToolsWiring, RunnerDefaultTimeoutOverIdleCeilingThrows) {
  Blackboard bb;
  Manifest m = mk("Tools\n{\n  timeout_s = 2000\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
TEST(ToolsWiring, InRangeTimeoutsBoot) {
  Blackboard bb;
  Manifest m = mk("Tool = ok\n{\n  native = /bin/true\n  timeout_s = 600\n}\n"
                  "Tools\n{\n  background_timeout_s = 7200\n}\n");
  Agent a = build_agent(bb, m);                    // 600 < 900 boots; bg timeout is EXEMPT
  SUCCEED();
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add to `CMakeLists.txt` next to the other test sources:

```cmake
target_sources(hades_tests PRIVATE tests/test_tools_wiring.cpp)
```

Expected: the two THROW tests FAIL (no invariant yet → agent builds fine).

- [ ] **Step 3: Implement.**

**(a)** `src/apps/tool_runner/tool_runner.cpp` `on_start` — accept the house-convention key (keep the legacy `timeout` read):

```cpp
void ToolRunner::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("timeout"))
    set_pos_double_on_string(cfg.kv.at("timeout"), timeout_s_);
  if (cfg.kv.count("timeout_s"))
    set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s_);   // Tools block key
  // Build the describe/spec cache ONCE here, so we never re-spawn a tool's
  // `describe` subprocess on each TOOL_REQUEST.
  reg_.warm(timeout_s_);
}
```

**(b)** `app/agent_wiring.cpp` — `wire_agent` (file-local) gains a trailing defaulted parameter `const Block& tools_cfg = Block{}` (after the existing last parameter), and its step-1 block changes to:

```cpp
  if (a.tools) {
    for (const auto& t : tools_resolved) a.tools->add_tool(t);
    a.tools->on_start(tools_cfg, bb);
    a.tools->on_attach(bb);
  }
```

**(c)** Manifest overload — directly AFTER the existing `turn_idle_timeout_s <= llm_timeout_s` MalConfig block, add:

```cpp
  // Tool-offload invariant extension: tools now run OFF the pump thread, so a silent
  // stretch can be a foreground tool run — the idle ceiling must outlast EVERY
  // foreground-effective tool timeout or run_until would abandon a slow-but-alive tool
  // mid-flight. Background runs are exempt (no run_until waits on them; delivery is
  // cross-turn via the BG_TASKS fold).
  const auto tools_cfg_blocks = m.of("Tools");
  const Block tools_cfg = tools_cfg_blocks.empty() ? Block{} : tools_cfg_blocks.front();
  double max_fg = 30.0;                                  // ToolRunner default timeout
  std::string max_fg_src = "the tool-runner default timeout_s";
  if (tools_cfg.kv.count("timeout_s")) {
    double v = max_fg;
    set_pos_double_on_string(tools_cfg.kv.at("timeout_s"), v);
    if (v > max_fg) { max_fg = v; max_fg_src = "Tools timeout_s"; }
  }
  for (const Block& t : m.of("Tool")) {
    double tt = 0.0;
    if (t.kv.count("timeout_s")) set_pos_double_on_string(t.kv.at("timeout_s"), tt);
    if (tt > max_fg) { max_fg = tt; max_fg_src = "Tool '" + t.name + "' timeout_s"; }
  }
  if (turn_idle_timeout_s <= max_fg) {
    auto fmt2 = [](double d) { std::ostringstream o; o << d; return o.str(); };
    throw MalConfig("turn_idle_timeout_s (" + fmt2(turn_idle_timeout_s) +
                    ") must be greater than " + max_fg_src + " (" + fmt2(max_fg) +
                    ") — a slow-but-alive offloaded tool would be abandoned mid-flight");
  }
```

**(d)** Executor block — bump the pool and give ToolRunner the executor (comment updated to name the new consumers):

```cpp
  // Live path only: worker pool for every offloaded blocking call — the LLM think, the
  // FOREGROUND tool run, background tool tasks (up to max_background), auto-extract
  // reviews, the embedding index, bridge share pushes. 8 threads = 1 llm + 1 fg tool +
  // 4 bg (default cap) + 2 slack; idle workers only cost a blocked thread.
  constexpr unsigned kExecutorThreads = 8;
  a.executor = std::make_unique<Executor>(kExecutorThreads);
  if (a.llm) a.llm->set_executor(a.executor.get());
  if (a.tools) a.tools->set_executor(a.executor.get());
```

**(e)** Pass the block through the Manifest overload's `wire_agent(...)` call: append `tools_cfg` as the new last argument.

- [ ] **Step 4: Build + test.** `-R ToolsWiring` → 3 pass; FULL suite → 763/763 (test-overload path passes `Block{}` by default → unchanged; dev.hades lock tests unaffected — its ask_agent/tool timeouts are all < 900).
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.cpp src/apps/tool_runner/tool_runner.cpp tests/test_tools_wiring.cpp CMakeLists.txt
git commit -m "feat: wire tool offload — Tools block, extended idle invariant, executor on ToolRunner"
```

---

## Task 4: Background execution (`background: true`)

**Files:**
- Modify: `include/hades/module/tool_runner.h` (bg members + `trunc_utf8_bytes` helper), `src/apps/tool_runner/tool_runner.cpp` (background branch, `BG_DONE`/`BG_TASKS`, schema append in `ensure_warm`, config keys in `on_start`)
- Test: `tests/test_tool_offload.cpp` (append)

**Interfaces:**
- Consumes: `execute_tool` (Task 2), `Executor::submit`, `Tools` block keys reaching `on_start` (Task 3).
- Produces: every announced ToolSpec schema gains a `background` boolean property; `background:true` + executor → immediate `TOOL_RESULT {ok:true, content:{started:true, task_id:"bg-N", note}}` (epoch echoed) then worker → `BG_DONE {task_id, ok, content}` → pump-thread ring → `BG_TASKS` (latest-value string, header `Background tasks (started earlier with background:true):`, empty registry → `""`). Task 5's Arbiter fold reads `BG_TASKS`. Public `inline std::string trunc_utf8_bytes(std::string, std::size_t)` in `tool_runner.h`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_tool_offload.cpp`:

```cpp
TEST(BackgroundTools, TruncUtf8Bytes) {
  EXPECT_EQ(trunc_utf8_bytes("short", 10), "short");
  std::string s(5, 'a'); s += "\xCE\xB1";                    // 5 ascii + 2-byte U+03B1 = 7 bytes
  EXPECT_EQ(trunc_utf8_bytes(s, 6), std::string(5, 'a') + " (truncated)");  // walks back over the split codepoint
  EXPECT_EQ(trunc_utf8_bytes(s, 7), s);                      // exact fit: untouched
  EXPECT_EQ(trunc_utf8_bytes(std::string(10, 'b'), 4), "bbbb (truncated)");
}

TEST(BackgroundTools, SchemaGainsBackgroundProperty) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("d", 0.0);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  auto specs = tr.registry().specs();
  ASSERT_EQ(specs.size(), 1u);
  ASSERT_TRUE(specs[0].schema["properties"].contains("background"));
  EXPECT_EQ(specs[0].schema["properties"]["background"]["type"], "boolean");
}

TEST(BackgroundTools, StartedResultThenBgDoneThenBgTasks) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("e", 0.2);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  std::string tasks;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.subscribe("BG_TASKS", [&](const Entry& e) { if (e.value.is_string()) tasks = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", {{"background", true}}}, {"epoch", 3}},
          "arbiter");
  bb.pump();                                       // the started-result is IMMEDIATE
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_TRUE(result["content"].value("started", false));
  const std::string tid = result["content"].value("task_id", "");
  EXPECT_EQ(tid.rfind("bg-", 0), 0u);
  EXPECT_EQ(result.value("epoch", 0), 3);
  EXPECT_NE(tasks.find(tid + " · slow · running"), std::string::npos);
  ASSERT_TRUE(bb.run_until(
      [&] { return tasks.find("finished ok") != std::string::npos; }, 5.0));
  EXPECT_NE(tasks.find("slow done"), std::string::npos);     // real output in the block
}

TEST(BackgroundTools, ToolNeverSeesBackgroundArgAndNoExecutorFallsBackInline) {
  // Reflective tool: reports LEAKED if the background flag reaches its stdin.
  const std::string p =
      ::testing::TempDir() + "/echo_args_" + std::to_string(::getpid()) + ".sh";
  {
    std::ofstream f(p);
    f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
      << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"echoargs\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
      << "*background*) echo '{\"ok\":false,\"result\":{\"error\":\"LEAKED\"}}' ;;\n"
      << "*) echo '{\"ok\":true,\"result\":{\"clean\":true}}' ;;\nesac\n";
  }
  ::chmod(p.c_str(), 0755);
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "echoargs"; b.kv["native"] = p;
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  // NO executor: the background flag is ignored (inline foreground) but still STRIPPED.
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "echoargs"}, {"args", {{"background", true}, {"x", 1}}}},
          "arbiter");
  bb.pump();
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_TRUE(result["content"].value("clean", false));      // real output, not {started}
}

TEST(BackgroundTools, CapRefusesOverMaxBackground) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("f", 1.0);
  tr.add_tool(b);
  Block cfg; cfg.kv["max_background"] = "1";
  tr.on_start(cfg, bb);
  tr.on_attach(bb);
  std::vector<nlohmann::json> results;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { results.push_back(e.value); });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", {{"background", true}}}}, "arbiter");
  bb.post("TOOL_REQUEST",
          {{"id", "t2"}, {"tool", "slow"}, {"args", {{"background", true}}}}, "arbiter");
  bb.pump();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_TRUE(results[0]["content"].value("started", false));
  EXPECT_FALSE(results[1].value("ok", true));                // cap 1: second refused
  EXPECT_NE(results[1]["content"].value("error", "").find("too many background"),
            std::string::npos);
  // Teardown note: ex (declared last) joins the 1s worker; its BG_DONE posts to the
  // still-alive bb and is simply never pumped — that's fine.
}

TEST(BackgroundTools, FinishedRingKeepsLastFiveAndTruncatesOutput) {
  // Big-output tool: ~3000-char result field -> the finished entry must carry the marker.
  const std::string p =
      ::testing::TempDir() + "/big_out_" + std::to_string(::getpid()) + ".sh";
  {
    std::ofstream f(p);
    f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
      << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"big\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
      << "*) printf '{\"ok\":true,\"result\":{\"big\":\"'\n"
      << "   i=0; while [ $i -lt 300 ]; do printf 'ABCDEFGHIJ'; i=$((i+1)); done\n"
      << "   printf '\"}}\\n' ;;\nesac\n";
  }
  ::chmod(p.c_str(), 0755);
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "big"; b.kv["native"] = p;
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  std::string tasks;
  bb.subscribe("BG_TASKS", [&](const Entry& e) { if (e.value.is_string()) tasks = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  for (int i = 0; i < 6; ++i) {                    // sequential: never trips the cap
    bb.post("TOOL_REQUEST",
            {{"id", "t" + std::to_string(i)}, {"tool", "big"},
             {"args", {{"background", true}}}},
            "arbiter");
    const std::string want = "bg-" + std::to_string(i) + " · big · finished ok";
    ASSERT_TRUE(bb.run_until([&] { return tasks.find(want) != std::string::npos; }, 5.0))
        << "task " << i;
  }
  EXPECT_EQ(tasks.find("bg-0 ·"), std::string::npos);        // ring cap 5: oldest dropped
  EXPECT_NE(tasks.find("bg-5 ·"), std::string::npos);
  EXPECT_NE(tasks.find(" (truncated)"), std::string::npos);  // 3000-char output capped at 2000B
}
```

- [ ] **Step 2: Run — expect FAIL** (compile: no `trunc_utf8_bytes`; then behavior failures).

- [ ] **Step 3: Implement.**

`include/hades/module/tool_runner.h` — add includes `<chrono>`, `<cstdint>`, `<deque>`, then above the class:

```cpp
// Byte-cap a string on a UTF-8 boundary: walk back over continuation bytes so the cut
// never splits a codepoint (the auto_extract trunc_utf8 pattern), then mark the cut.
// Header-inline so tests exercise the boundary walk directly.
inline std::string trunc_utf8_bytes(std::string s, std::size_t cap) {
  if (s.size() <= cap) return s;
  s.resize(cap);
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  return s + " (truncated)";
}
```

and inside the class (private, after `executor_`):

```cpp
  // Background-task registry — mutated ONLY on the pump thread (the BG_DONE subscriber);
  // workers just post. In-memory, process-lifetime (documented v1: tasks die on restart).
  struct BgRunning { std::string tool; std::chrono::steady_clock::time_point started; };
  struct BgFinished { std::string task_id, tool, output; bool ok = false; };
  void on_bg_done_(const Entry& e);   // running -> finished ring -> post BG_TASKS
  void post_bg_tasks_();              // format + post the BG_TASKS latest-value block
  std::map<std::string, BgRunning> bg_running_;
  std::deque<BgFinished> bg_finished_;   // newest at back; capped at 5
  std::uint64_t bg_counter_ = 0;
  unsigned max_background_ = 4;
  double bg_timeout_s_ = 600.0;
```

(`include/hades/entry.h` is already reachable via `hades/module.h`; add `#include "hades/entry.h"` explicitly if the compiler asks.)

`src/apps/tool_runner/tool_runner.cpp`:

**(a)** `on_start` gains the two background keys (after the `timeout_s` read from Task 3):

```cpp
  if (cfg.kv.count("background_timeout_s"))
    set_pos_double_on_string(cfg.kv.at("background_timeout_s"), bg_timeout_s_);
  if (cfg.kv.count("max_background")) {
    double v = 0.0;
    set_pos_double_on_string(cfg.kv.at("max_background"), v);
    if (v >= 1.0 && v <= 64.0) max_background_ = static_cast<unsigned>(v);  // bound the cast
  }
```

**(b)** In the `TOOL_REQUEST` subscriber, right AFTER the `args` extraction and BEFORE the `find_by_tool_name` lookup, read + strip the harness-owned flag:

```cpp
    // Background opt-in: harness-owned — strip it in ALL paths so the tool binary / MCP
    // server never sees it. Non-bool value = absent (house rule).
    bool background = false;
    if (auto ba = args.find("background"); ba != args.end()) {
      if (ba->is_boolean()) background = ba->get<bool>();
      args.erase(ba);
    }
```

**(c)** After the `entry`/`real`/`timeout` resolution and BEFORE the foreground `run` lambda, add the background branch:

```cpp
    if (background && executor_) {
      if (bg_running_.size() >= max_background_) {
        nlohmann::json out{
            {"id", id}, {"ok", false},
            {"content",
             {{"error", "too many background tasks (" + std::to_string(bg_running_.size()) +
                            " running, cap " + std::to_string(max_background_) +
                            ") — wait for one to finish"}}}};
        if (has_epoch) out["epoch"] = epoch;
        bb_->post("TOOL_RESULT", out, "tool_runner", id);
        return;
      }
      const std::string task_id = "bg-" + std::to_string(bg_counter_++);
      bg_running_.emplace(task_id, BgRunning{name, std::chrono::steady_clock::now()});
      // Builds need longer than the interactive per-tool cap; the invariant EXEMPTS this
      // timeout (no run_until waits on a background task).
      const double bg_timeout = std::max(timeout, bg_timeout_s_);
      Blackboard* bbg = bb_;
      executor_->submit([bbg, task_id, name, args, entry, real, bg_timeout] {
        auto [ok, content] = execute_tool(entry, name, real, args, bg_timeout);
        // Epoch-FREE by design: completion is cross-turn; the BG_TASKS fold delivers it.
        bbg->post("BG_DONE", {{"task_id", task_id}, {"ok", ok}, {"content", content}},
                  "tool_runner", task_id);
      });
      post_bg_tasks_();   // the running entry is visible to the very next start_turn
      nlohmann::json out{{"id", id}, {"ok", true},
                         {"content",
                          {{"started", true},
                           {"task_id", task_id},
                           {"note", "result will appear in the Background tasks block"}}}};
      if (has_epoch) out["epoch"] = epoch;
      bb_->post("TOOL_RESULT", out, "tool_runner", id);
      return;
    }
```

(`background && !executor_` deliberately falls through to the foreground path — the flag
was already stripped, so the call runs inline as if foreground.)

**(d)** `on_attach` subscribes the completion event (after the TOOL_REQUEST subscription):

```cpp
  bb.subscribe("BG_DONE", [this](const Entry& e) { on_bg_done_(e); });
```

**(e)** New member functions (below `on_attach`, above the registry section):

```cpp
// Pump-thread only (bus contract): fold a worker's completion into the registry and
// republish the fold block. Unknown/duplicate task_id -> ignore.
void ToolRunner::on_bg_done_(const Entry& e) {
  if (!e.value.is_object()) return;
  auto it = bg_running_.find(e.value.value("task_id", ""));
  if (it == bg_running_.end()) return;
  BgFinished f;
  f.task_id = it->first;
  f.tool = it->second.tool;
  f.ok = e.value.value("ok", false);
  const auto content =
      e.value.contains("content") ? e.value["content"] : nlohmann::json::object();
  f.output = trunc_utf8_bytes(
      content.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), 2000);
  bg_running_.erase(it);
  bg_finished_.push_back(std::move(f));
  while (bg_finished_.size() > 5) bg_finished_.pop_front();   // ring cap
  post_bg_tasks_();
}

void ToolRunner::post_bg_tasks_() {
  std::string block;
  if (!bg_running_.empty() || !bg_finished_.empty()) {
    block = "Background tasks (started earlier with background:true):";
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [tid, r] : bg_running_) {
      const auto secs =
          std::chrono::duration_cast<std::chrono::seconds>(now - r.started).count();
      block += "\n- " + tid + " · " + r.tool + " · running (" +
               std::to_string(secs) + "s)";
    }
    for (const auto& f : bg_finished_)
      block += "\n- " + f.task_id + " · " + f.tool +
               (f.ok ? " · finished ok: " : " · FAILED: ") + f.output;
  }
  bb_->post("BG_TASKS", block, "tool_runner");   // "" when the registry is empty
}
```

**(f)** Schema append — in `ToolRegistry::ensure_warm`, after the whole tool loop (just before the function's closing brace):

```cpp
  // Background opt-in (harness-owned): every announced tool accepts `background:true` —
  // the ToolRunner strips it before the subprocess/MCP call, so tool binaries and MCP
  // servers never see it. Injected here so native AND discovered-MCP specs get it.
  for (auto& sp : specs_) {
    if (!sp.schema.is_object()) sp.schema = nlohmann::json::object();
    if (!sp.schema.contains("properties") || !sp.schema["properties"].is_object())
      sp.schema["properties"] = nlohmann::json::object();
    sp.schema["properties"]["background"] =
        {{"type", "boolean"},
         {"description",
          "run in background: returns {started, task_id} immediately; the result appears "
          "in the 'Background tasks' block when done (default false)"}};
  }
```

- [ ] **Step 4: Build + test.** `-R "ToolOffload|BackgroundTools"` → 9 pass; FULL suite → 769/769.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/tool_runner.h src/apps/tool_runner/tool_runner.cpp tests/test_tool_offload.cpp
git commit -m "feat: background tool execution — background:true, BG_DONE/BG_TASKS registry"
```

---

## Task 5: Arbiter BG_TASKS fold + docs ship

**Files:**
- Modify: `src/apps/arbiter/arbiter.cpp` (`start_turn`, directly AFTER the todo fold block ~line 229, BEFORE the peer folds)
- Test: `tests/test_arbiter.cpp` (append)
- Modify: `prompts/soul.md`, `docs/manifest-reference.md`, `CLAUDE.md`, `manifests/dev.hades`

**Interfaces:**
- Consumes: `BG_TASKS` latest-value string (Task 4).
- Produces: the block folded verbatim into the leading system message (it carries its own header line); absent/non-string/empty → nothing.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp`:

```cpp
TEST(Arbiter, FoldsBgTasksIntoSystemMessage) {
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL"); a.on_attach(bb);
  bb.post("BG_TASKS",
          "Background tasks (started earlier with background:true):\n- bg-0 · shell · running (3s)",
          "tool_runner");
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("bg-0 · shell · running"), std::string::npos);
  EXPECT_LT(sys.find("SOUL"), sys.find("Background tasks"));
}
TEST(Arbiter, EmptyOrMissingBgTasksInjectsNothing) {
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL"); a.on_attach(bb);
  bb.post("BG_TASKS", "", "tool_runner");   // registry empty -> module posts ""
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["content"], "SOUL");
}
```

- [ ] **Step 2: Run — expect FAIL** (no fold yet).

- [ ] **Step 3: Implement the fold.** In `start_turn()`, after the todo-fold `if (!todo_path_.empty()) {...}` block:

```cpp
  // Background tasks: fold the ToolRunner's BG_TASKS block (latest-value; posted on task
  // start and completion) — running + finished background work, the cross-turn delivery
  // half of background:true. The block carries its own header. Rebuilt every start_turn
  // (tool-loop continuations included), so a completion landing mid-turn is visible on
  // the next LLM round-trip. Absent, non-string, or empty -> no block.
  if (auto bg = bb_->get("BG_TASKS"); bg && bg->value.is_string()) {
    const std::string tasks = bg->value.get<std::string>();
    if (!tasks.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += tasks;
    }
  }
```

- [ ] **Step 4: Build + test.** `-R Arbiter` → all pass; FULL suite → 771/771.

- [ ] **Step 5: Docs.**

**(a)** `prompts/soul.md` — append after the "## Working through multi-step tasks" section:

```markdown
## Background tasks

Long operations — builds, `ask_agent` delegations, large downloads — accept an extra
`background: true` argument. The call returns `{started, task_id}` immediately; keep
working or answer the user, and the result appears in the "Background tasks" block of
your context on a later turn. Check that block before re-running work you already
started. Never background quick reads or writes — they finish instantly and
backgrounding them only delays their results.
```

**(b)** `docs/manifest-reference.md` — add a new numbered section (next free number, after the `Search` block section) titled "`Tools` block — tool-runner tuning (optional)":

```markdown
| key | default | meaning |
|---|---|---|
| `timeout_s` | 30 | runner-wide default per-tool subprocess timeout (a `Tool` block's `timeout_s` overrides per tool) |
| `max_background` | 4 | max concurrently RUNNING background tasks (bounds 1..64); an over-cap `background:true` call is refused with an error result |
| `background_timeout_s` | 600 | background run timeout — the effective cap is `max(per-tool timeout_s, background_timeout_s)` |

Every announced tool accepts an extra `background: true` argument (harness-owned —
stripped before the subprocess/MCP call; tool binaries never see it). The call
immediately returns `{started, task_id}`; the result is folded into the system prompt's
"Background tasks" block (running + last 5 finished, output truncated to 2000 bytes) on
later turns. Background tasks die with the process (not persisted); a background
`write_file` does not update the staleness guard (the next edit is refused stale → the
agent re-reads — self-healing).

**Launch invariant (tool-offload):** `turn_idle_timeout_s` must exceed `llm_timeout_s`,
`Tools.timeout_s`, and every `Tool` block's `timeout_s` — boot fails with `MalConfig`
otherwise. `background_timeout_s` is exempt (nothing waits on a background run).
```

Also add one line to the Session-block section's `turn_idle_timeout_s` row noting the extended invariant.

**(c)** `manifests/dev.hades` — add a commented block near the Tool lines:

```
# Tools            # tool-runner tuning (optional; defaults shown)
# {
#   timeout_s = 30
#   max_background = 4
#   background_timeout_s = 600
# }
```

**(d)** `CLAUDE.md` — in the "### CC tool-gap wave" section, strike item 4 in the gap list (`~~Background tool execution~~ — SHIPPED`) and add a short entry to the wave section (or a sibling section) recording: offload = LLM pattern in ToolRunner; epoch on TOOL_RESULT + orphan pop close the arbiter.cpp deferral; extended idle invariant (idle > every foreground tool timeout, bg exempt); `background:true` on every tool schema, harness-stripped; `BG_DONE`/`BG_TASKS`/fold; `Tools` block keys; `kExecutorThreads` 8; v1 edges (restart kills bg tasks, bg write_file staleness self-heal, bg save_skill misses rescan). Update the test count in the header line.

- [ ] **Step 6: Full verification, BOTH lanes.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan --output-on-failure
```

Expected: ALL green in both (771/771). TSan matters here: the offload worker + BG_DONE path is new cross-thread traffic.

- [ ] **Step 7: Commit.**

```bash
git add src/apps/arbiter/arbiter.cpp tests/test_arbiter.cpp prompts/soul.md docs/manifest-reference.md CLAUDE.md manifests/dev.hades
git commit -m "feat: Arbiter BG_TASKS fold + soul/manifest/CLAUDE docs for background tools"
```

---

## Verification (end-to-end)

1. Full suite both lanes (ASan+UBSan `build/`, TSan `build-tsan/`): 771/771.
2. Manual live smoke (Vaios, `dev.local.hades` + `HADES_API_KEY`):
   ```
   user> run "sleep 20 && echo done" in the background, then tell me a joke
     -> shell confirm prompt (confirm-band unchanged) -> approve
     -> immediate {started, task_id} -> joke arrives without a 20s stall
   user> what happened to that task?
     -> "Background tasks" block shows bg-0 finished ok: ...done...
   user> ask pi0 something with background:true
     -> asker stays responsive during the peer turn
   ```
3. Bus-liveness check: during a long foreground tool, a Telegram message no longer waits for the tool to finish before the TurnGate rejection/queue behaves as before (turn semantics unchanged — only the pump is live).

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus), per-task cpp-reviewer, final whole-branch review, then finishing-a-development-branch (merge ff to main — push ONLY on Vaios's word).
