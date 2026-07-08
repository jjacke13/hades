# hades Reactive `when=` Trigger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Heartbeat entries (static manifest + dynamic `schedule_task`) can fire on a Blackboard **condition** (`when = KEY changes|is|not|above|below …`) instead of a schedule — edge-triggered, cooldown-bounded, evaluated on the existing ~30s tick against latest values.

**Architecture:** A pure condition lib (`when.h/.cpp`, the `cron.h` sibling — compiled into `hades_core` AND the `schedule_task` binary). `HeartbeatEntry` gains a `when` kind with per-entry edge state; `tick()` routes when-entries to a new `maybe_fire_when_` (bypasses the minute stamp — edge semantics + cooldown do the dedup; a busy-skip does not consume the edge). The store/tool/list gain the 4th kind. `SelfScheduleGuard` additionally hard-vetoes `schedule_task` on `peer:` origins unconditionally (hole fix).

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest.

## Global Constraints

- **Every build/test runs inside `nix develop`:** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **529/529 green**. Each task keeps the suite green.
- Branch `feat/when-trigger` (created; spec committed `da106e3`). Spec: `docs/superpowers/specs/2026-07-08-when-trigger-design.md`.
- Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By, NO Generated-with.**
- **Condition grammar is exactly:** `<KEY> changes` | `<KEY> is <str>` | `<KEY> not <str>` | `<KEY> above <n>` | `<KEY> below <n>` — whitespace-split; `is`/`not` operands may contain spaces (operand = the remainder of the string); `above`/`below` operands must parse as a number. Malformed → invalid.
- **Value-to-string for `is`/`not`:** a JSON string compares as its raw content (`get<std::string>()`); any other JSON value compares as its compact `dump()`.
- **Edge semantics:** `changes` arms on first observation (no fire), fires once per subsequent change; `is/not/above/below` fire on the false→true edge, re-arm on false; already-true at first evaluation fires once. **A busy-skip (fire_ returned false) must NOT advance edge state** (retry next tick). A fire that RAN advances state even on turn-timeout.
- **`cooldown_s`** default **60**: after a fire that ran, suppress further fires of that entry until `now_epoch >= last_fire + cooldown_s`; edges during cooldown are **absorbed** (state advances, no fire, no queueing).
- **Exactly one of** `schedule` | `when` per entry (static: `MalConfig`; tool: adds `when` to the existing exactly-one set `schedule|in_minutes|at`). `min_interval_s` does NOT apply to `when` tasks; `max_tasks` DOES.
- **`SelfScheduleGuard`:** `peer:`-origin `schedule_task` → hard veto ALWAYS (even with `allow_self_schedule=true`); `heartbeat:` origin governed by `allow_self_schedule` (unchanged); human free (unchanged).
- Working-tree hygiene: **never stage** `manifests/dev.hades`, `manifests/pi.hades`, `memory/facts.md`; never `git add -A`. The Task 4 dev.hades edit is controller-handled (backup→checkout→edit→commit→restore).
- Pump/timer-thread handlers and tool mains never throw on adversarial input.

---

## File Structure

```
include/hades/heartbeat/when.h          T1  WhenCond + parse_when/when_valid/when_holds
src/apps/heartbeat/when.cpp             T1  impl (pure, tolerant)
tests/test_when.cpp                     T1
include/hades/heartbeat/cron_store.h    T2  CronTask += when, cooldown_s (modify)
src/apps/heartbeat/cron_store.cpp       T2  (de)serialize the new fields (modify)
tools/schedule_task_main.cpp            T2  when = 4th exclusive kind (modify)
tools/list_tasks_main.cpp               T2  display when (modify)
src/behaviors/standard_behaviors.cpp    T2  SelfScheduleGuard peer-origin veto (modify)
include/hades/objective/self_schedule_guard.h  T2  doc comment (modify)
tests/test_cron_store.cpp               T2  (append)
tests/test_schedule_task_tool.cpp       T2  (append)
tests/test_list_cancel_tools.cpp        T2  (append)
tests/test_self_schedule_guard.cpp      T2  (append)
include/hades/module/heartbeat_module.h T3  when fields + WhenState + maybe_fire_when_ (modify)
src/apps/heartbeat/heartbeat.cpp        T3  eval/edge/cooldown/reload-state (modify)
tests/test_heartbeat_module.cpp         T3  (append)
app/agent_wiring.cpp                    T4  when XOR schedule parse, cooldown_s, when_valid (modify)
tests/test_heartbeat_wiring.cpp         T4  (append)
manifests/dev.hades (controller), docs/manifest-reference.md, prompts/soul.md, CLAUDE.md  T4
CMakeLists.txt                          T1,T2 (sources)
```

---

## Task 1: `when` condition lib (pure)

**Files:** Create `include/hades/heartbeat/when.h`, `src/apps/heartbeat/when.cpp`, `tests/test_when.cpp`. Modify `CMakeLists.txt`.

**Interfaces — Produces (all `namespace hades`):**
- `struct WhenCond { std::string key; enum class Op { Changes, Is, Not, Above, Below } op = Op::Changes; std::string operand; };`
- `std::optional<WhenCond> parse_when(const std::string& expr);`
- `bool when_valid(const std::string& expr);`
- `bool when_holds(const WhenCond&, const nlohmann::json* value);` — for Is/Not/Above/Below; `nullptr` → false; Changes → always false (evaluated statefully by the module).

- [ ] **Step 1: Write the failing tests** `tests/test_when.cpp`:

```cpp
// tests/test_when.cpp — pure when-condition lib: parse, validate, evaluate
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/when.h"
using namespace hades;

TEST(When, ParsesAllFiveForms) {
  auto c = parse_when("PEER.pi0.card changes");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->key, "PEER.pi0.card");
  EXPECT_EQ(c->op, WhenCond::Op::Changes);

  c = parse_when("MISSION_STATE is returning home");   // operand may contain spaces
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Is);
  EXPECT_EQ(c->operand, "returning home");

  c = parse_when("MISSION_STATE not idle");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Not);

  c = parse_when("BUDGET_SPENT_USD above 0.8");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Above);
  EXPECT_EQ(c->operand, "0.8");

  c = parse_when("GPS_QUALITY below 4");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Below);
}

TEST(When, RejectsMalformed) {
  EXPECT_FALSE(parse_when("").has_value());
  EXPECT_FALSE(parse_when("KEY").has_value());                    // no op
  EXPECT_FALSE(parse_when("KEY frobnicates").has_value());        // unknown op
  EXPECT_FALSE(parse_when("KEY changes extra").has_value());      // changes takes no operand
  EXPECT_FALSE(parse_when("KEY is").has_value());                 // is needs an operand
  EXPECT_FALSE(parse_when("KEY above").has_value());              // threshold needs an operand
  EXPECT_FALSE(parse_when("KEY above lots").has_value());         // non-numeric threshold
  EXPECT_TRUE(when_valid("K is v"));
  EXPECT_FALSE(when_valid("K above nan-ish"));
}

TEST(When, HoldsStringEquality) {
  WhenCond c{"K", WhenCond::Op::Is, "idle"};
  nlohmann::json s = "idle";
  nlohmann::json other = "busy";
  EXPECT_TRUE(when_holds(c, &s));
  EXPECT_FALSE(when_holds(c, &other));
  EXPECT_FALSE(when_holds(c, nullptr));                            // absent key -> false
  c.op = WhenCond::Op::Not;
  EXPECT_FALSE(when_holds(c, &s));
  EXPECT_TRUE(when_holds(c, &other));
  EXPECT_FALSE(when_holds(c, nullptr));                            // absent: not even "not"
}

TEST(When, HoldsNonStringValuesCompareAsDump) {
  WhenCond c{"K", WhenCond::Op::Is, "{\"a\":1}"};
  nlohmann::json obj = {{"a", 1}};
  EXPECT_TRUE(when_holds(c, &obj));                                // compact dump equality
}

TEST(When, HoldsNumericThresholds) {
  WhenCond above{"K", WhenCond::Op::Above, "0.8"};
  WhenCond below{"K", WhenCond::Op::Below, "4"};
  nlohmann::json n1 = 0.9, n2 = 0.8, n3 = 3, s = "5";
  EXPECT_TRUE(when_holds(above, &n1));
  EXPECT_FALSE(when_holds(above, &n2));                            // strict >
  EXPECT_TRUE(when_holds(below, &n3));
  EXPECT_FALSE(when_holds(above, &s));                             // non-number -> false, no throw
  EXPECT_FALSE(when_holds(above, nullptr));
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** Next to the `cron.cpp` lines in `CMakeLists.txt` add:

```cmake
target_sources(hades_core PRIVATE src/apps/heartbeat/when.cpp)
```

and next to the other test sources:

```cmake
target_sources(hades_tests PRIVATE tests/test_when.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/heartbeat/when.h`:

```cpp
// include/hades/heartbeat/when.h — pure reactive-trigger condition (KEY changes|is|not|above|below)
//
// The `when =` counterpart to cron.h: parses and evaluates the 5 keyword condition forms a Heartbeat
// entry may carry instead of a schedule. Keyword operators (never '='/'=='), because an inline
// manifest value containing '=' trips the one-kv-per-line fail-loud parser. Evaluation is pure and
// stateless here — the edge detection ("fire once per change / per false->true transition") lives in
// the HeartbeatModule, which owns the per-entry state. Tolerant: malformed -> nullopt/false, never
// throws. Compiled into hades_core AND the schedule_task binary (cron.h precedent).
#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
namespace hades {

struct WhenCond {
  std::string key;                    // the Blackboard key to watch
  enum class Op { Changes, Is, Not, Above, Below } op = Op::Changes;
  std::string operand;                // "" for Changes; string for Is/Not; numeric text for Above/Below
};

// Parse "<KEY> <op> [operand]". Is/Not take the REMAINDER of the string as the operand (spaces ok);
// Above/Below require a parseable number; Changes takes nothing. nullopt on any violation.
std::optional<WhenCond> parse_when(const std::string& expr);

// Fail-loud validator for wiring (MalConfig) and the schedule_task tool (ok:false).
bool when_valid(const std::string& expr);

// Evaluate Is/Not/Above/Below against a latest value (nullptr = key absent -> false).
// Changes always returns false here — it is stateful and evaluated by the module.
bool when_holds(const WhenCond& c, const nlohmann::json* value);

}  // namespace hades
```

`src/apps/heartbeat/when.cpp`:

```cpp
// src/apps/heartbeat/when.cpp — parse + evaluate the reactive when-condition (pure, tolerant)
#include "hades/heartbeat/when.h"
#include <cstdlib>
namespace hades {
namespace {
// The value as the string is/not compare against: raw content for a JSON string, compact dump else.
std::string value_text(const nlohmann::json& v) {
  return v.is_string() ? v.get<std::string>() : v.dump();
}
bool parse_number(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}
}  // namespace

std::optional<WhenCond> parse_when(const std::string& expr) {
  const std::size_t sp1 = expr.find(' ');
  if (sp1 == std::string::npos || sp1 == 0) return std::nullopt;
  WhenCond c;
  c.key = expr.substr(0, sp1);
  const std::size_t op_start = expr.find_first_not_of(' ', sp1);
  if (op_start == std::string::npos) return std::nullopt;
  const std::size_t sp2 = expr.find(' ', op_start);
  const std::string op = expr.substr(op_start, sp2 == std::string::npos ? std::string::npos
                                                                        : sp2 - op_start);
  std::string rest;
  if (sp2 != std::string::npos) {
    const std::size_t rest_start = expr.find_first_not_of(' ', sp2);
    if (rest_start != std::string::npos) rest = expr.substr(rest_start);
  }
  if (op == "changes") {
    if (!rest.empty()) return std::nullopt;              // changes takes no operand
    c.op = WhenCond::Op::Changes;
    return c;
  }
  if (rest.empty()) return std::nullopt;                 // every other op needs an operand
  c.operand = rest;
  if (op == "is")  { c.op = WhenCond::Op::Is;  return c; }
  if (op == "not") { c.op = WhenCond::Op::Not; return c; }
  double n;
  if (op == "above") { c.op = WhenCond::Op::Above; return parse_number(rest, n) ? std::optional(c) : std::nullopt; }
  if (op == "below") { c.op = WhenCond::Op::Below; return parse_number(rest, n) ? std::optional(c) : std::nullopt; }
  return std::nullopt;                                   // unknown op
}

bool when_valid(const std::string& expr) { return parse_when(expr).has_value(); }

bool when_holds(const WhenCond& c, const nlohmann::json* value) {
  if (!value) return false;                              // absent key: condition cannot hold
  switch (c.op) {
    case WhenCond::Op::Is:  return value_text(*value) == c.operand;
    case WhenCond::Op::Not: return value_text(*value) != c.operand;
    case WhenCond::Op::Above:
    case WhenCond::Op::Below: {
      if (!value->is_number()) return false;
      const double threshold = std::strtod(c.operand.c_str(), nullptr);   // validated at parse time
      const double v = value->get<double>();
      return c.op == WhenCond::Op::Above ? v > threshold : v < threshold;
    }
    case WhenCond::Op::Changes: return false;            // stateful; module-evaluated
  }
  return false;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R When` → pass; FULL suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/heartbeat/when.h src/apps/heartbeat/when.cpp tests/test_when.cpp CMakeLists.txt
git commit -m "feat: when-condition lib (parse/validate/evaluate the 5 keyword forms)"
```

---

## Task 2: store fields + tool 4th kind + list display + guard peer-veto

**Files:** Modify `include/hades/heartbeat/cron_store.h`, `src/apps/heartbeat/cron_store.cpp`, `tools/schedule_task_main.cpp`, `tools/list_tasks_main.cpp`, `src/behaviors/standard_behaviors.cpp`, `include/hades/objective/self_schedule_guard.h` (comment), `CMakeLists.txt`. Append tests to `tests/test_cron_store.cpp`, `tests/test_schedule_task_tool.cpp`, `tests/test_list_cancel_tools.cpp`, `tests/test_self_schedule_guard.cpp`.

**Interfaces:**
- `CronTask` gains `std::string when;` and `long long cooldown_s = 60;` (APPENDED — existing 8-field aggregate inits keep compiling). `add_record` serializes `when` (string for kind=="when", else null) and `cooldown_s` (always); `task_from_json` reads both guarded.
- `schedule_task` accepts `when` (string) as the 4th member of the exactly-one set; validates via `hades::when_valid`; optional `cooldown_s` (number ≥ 0, default 60); result `kind:"when"`, `when` echoed as `when`. `min_interval_s` not applied to `when`. The `describe` schema documents `when` + `cooldown_s` (this IS LLM API surface — one-line vocabulary: "KEY changes | KEY is <v> | KEY not <v> | KEY above <n> | KEY below <n>").
- `list_tasks` shows `when` for when-kind.
- `SelfScheduleGuard::veto`: `peer:`-origin `schedule_task` → hard veto ALWAYS (before the `allow_` early-return); heartbeat/human behavior unchanged.

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_cron_store.cpp`:

```cpp
TEST(CronStore, WhenKindRoundTrips) {
  CronTask t{"w1", "watch", "when", "", 0, "check it", true, 42};
  t.when = "PEER.pi0.card changes";
  t.cooldown_s = 120;
  auto v = fold_cron_store(add_record(t) + "\n");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].kind, "when");
  EXPECT_EQ(v[0].when, "PEER.pi0.card changes");
  EXPECT_EQ(v[0].cooldown_s, 120);
  // non-when kinds serialize when as null and default cooldown
  auto j = nlohmann::json::parse(add_record({"c1", "n", "cron", "* * * * *", 0, "p", false, 1}));
  EXPECT_TRUE(j["when"].is_null());
}

TEST(CronStore, MissingWhenFieldsDefaultTolerantly) {
  // A pre-when record (older store) folds with when="" and cooldown_s=60.
  const char* old_rec =
      R"({"op":"add","id":"a1","name":"n","kind":"cron","schedule":"* * * * *","fire_epoch":null,"prompt":"p","notify":false,"created":1})";
  auto v = fold_cron_store(std::string(old_rec) + "\n");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_TRUE(v[0].when.empty());
  EXPECT_EQ(v[0].cooldown_s, 60);
}
```

Append to `tests/test_schedule_task_tool.cpp` (reuse its `call_sched`/`fresh_store` helpers — read the file, match exactly):

```cpp
TEST(ScheduleTaskTool, WhenKindAccepted) {
  const std::string store = fresh_store("when");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"watch","prompt":"check","when":"PEER.pi0.card changes","notify":true}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "when");
  EXPECT_EQ(j["result"].value("when", ""), "PEER.pi0.card changes");
}

TEST(ScheduleTaskTool, MalformedWhenRejected) {
  const std::string store = fresh_store("badwhen");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"w","prompt":"p","when":"KEY frobnicates"}})");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(std::filesystem::exists(store));
}

TEST(ScheduleTaskTool, WhenJoinsExactlyOneSet) {
  const std::string store = fresh_store("whenexcl");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"w","prompt":"p","when":"K changes","schedule":"* * * * *"}})");
  EXPECT_FALSE(j.value("ok", true));   // two timing kinds -> refused
}

TEST(ScheduleTaskTool, WhenCooldownStoredAndDefaulted) {
  const std::string store = fresh_store("cool");
  ASSERT_TRUE(call_sched(store,
      R"({"call":"schedule_task","args":{"name":"w","prompt":"p","when":"K changes","cooldown_s":300}})").value("ok", false));
  std::ifstream f(store); std::stringstream s; s << f.rdbuf();
  auto v = hades::fold_cron_store(s.str());
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].cooldown_s, 300);
}
```

Append to `tests/test_list_cancel_tools.cpp`:

```cpp
TEST(ListTasksTool, WhenKindShowsCondition) {
  const char* rec =
      R"({"op":"add","id":"w1","name":"watch","kind":"when","schedule":null,"fire_epoch":null,"when":"BUDGET_SPENT_USD above 0.8","cooldown_s":60,"prompt":"p","notify":true,"created":1})";
  const std::string store = store_with("when", std::string(rec) + "\n");
  ProcResult r = run_subprocess({LIST_TASKS_BIN, store}, R"({"call":"list_tasks"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["tasks"].size(), 1u);
  EXPECT_EQ(j["result"]["tasks"][0].value("when", ""), "BUDGET_SPENT_USD above 0.8");
}
```

Append to `tests/test_self_schedule_guard.cpp`:

```cpp
TEST(SelfScheduleGuard, PeerOriginAlwaysVetoed) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "peer:pi0", "bridge");
  SelfScheduleGuard strict(false), permissive(true);
  EXPECT_TRUE(strict.veto(bb, sched("schedule_task")).vetoed);
  EXPECT_TRUE(permissive.veto(bb, sched("schedule_task")).vetoed);   // allow_self_schedule does NOT open peers
  EXPECT_FALSE(permissive.veto(bb, sched("list_tasks")).vetoed);     // read path untouched
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** `hades-schedule-task` must now also compile the when lib — extend its `add_executable`:

```cmake
add_executable(hades-schedule-task tools/schedule_task_main.cpp
               src/apps/heartbeat/cron_store.cpp src/apps/heartbeat/cron.cpp
               src/apps/heartbeat/when.cpp)
```

- [ ] **Step 3: Implement.**
  1. `cron_store.h`: append to `CronTask`: `std::string when;  // reactive condition (kind=="when"); "" otherwise` and `long long cooldown_s = 60;  // min seconds between fires (when kind)`.
  2. `cron_store.cpp` `add_record`: add `{"when", t.kind == "when" ? nlohmann::json(t.when) : nlohmann::json()}` and `{"cooldown_s", t.cooldown_s}`. `task_from_json`: guarded reads — `if (j.contains("when") && j["when"].is_string()) t.when = ...;` and `if (j.contains("cooldown_s") && j["cooldown_s"].is_number_integer()) t.cooldown_s = ...;`.
  3. `schedule_task_main.cpp`: `#include "hades/heartbeat/when.h"`. Add `has_when` to the exactly-one check (`has_sched + has_in + has_at + has_when != 1`). New branch: validate `hades::when_valid` (else `fail("invalid when condition: ...")`), `t.kind = "when"; t.when = <expr>; when_result = expr`. Read optional `cooldown_s` (number, `>= 0`, default 60; reject negative) into `t.cooldown_s` (when-kind only). Result gains `{"when", t.when}` for when-kind. Update the `describe` description + schema (`when` string property documented with the 5-form vocabulary one-liner; `cooldown_s` number).
  4. `list_tasks_main.cpp`: in the task loop, `else if (t.kind == "when") e["when"] = t.when;` (before the existing `else e["at"] = ...` — make the chain `cron → schedule / when → when / else → at`).
  5. `standard_behaviors.cpp` `SelfScheduleGuard::veto` — peer veto BEFORE the `allow_` early-return:

```cpp
VetoResult SelfScheduleGuard::veto(const Blackboard& bb, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall || a.tool != "schedule_task") return {};
  auto e = bb.get("TURN_ORIGIN");
  const std::string origin =
      (e && e->value.is_string()) ? e->value.get<std::string>() : std::string{};
  // A peer must NEVER plant standing work on this agent — not switchable (bridge philosophy:
  // peer powers = the receiver's unconfirmed powers, and standing tasks are excluded by design).
  if (origin.rfind("peer:", 0) == 0)
    return {true, "a peer-driven turn cannot schedule tasks on this agent", false};
  if (allow_) return {};                                  // operator opted in: ticks may self-schedule
  if (origin.rfind("heartbeat:", 0) == 0)
    return {true, "a heartbeat-driven turn cannot self-schedule (allow_self_schedule=false)", false};
  return {};
}
```

  6. `self_schedule_guard.h` header comment: add one line about the unconditional peer veto.

- [ ] **Step 4: Build + test.** `-R "CronStore|ScheduleTaskTool|ListTasksTool|SelfScheduleGuard"` → pass; FULL suite green (existing guard tests must still pass — the human/heartbeat paths are behavior-identical).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/heartbeat/cron_store.h src/apps/heartbeat/cron_store.cpp tools/schedule_task_main.cpp tools/list_tasks_main.cpp src/behaviors/standard_behaviors.cpp include/hades/objective/self_schedule_guard.h tests/test_cron_store.cpp tests/test_schedule_task_tool.cpp tests/test_list_cancel_tools.cpp tests/test_self_schedule_guard.cpp CMakeLists.txt
git commit -m "feat: when-kind tasks in store/tools; SelfScheduleGuard vetoes peer-origin scheduling"
```

---

## Task 3: HeartbeatModule — evaluate `when` entries (edge, cooldown, reload state)

**Files:** Modify `include/hades/module/heartbeat_module.h`, `src/apps/heartbeat/heartbeat.cpp`. Append tests to `tests/test_heartbeat_module.cpp`.

**Interfaces:**
- `HeartbeatEntry` APPENDS: `std::string when;` `long long cooldown_s = 60;` `bool when_armed = false;` `std::string when_last_dump;` `bool when_was_true = false;` `long long when_last_fire_epoch = 0;` (existing aggregate inits keep compiling).
- `tick()` routes: `e.when.empty()` → existing `maybe_fire_`; else → new `maybe_fire_when_(e, now_epoch, dynamic)` (no minute-stamp gate — edge + cooldown are the dedup).
- Dynamic when-entries carry edge state across reloads via `struct WhenState { bool armed=false; std::string last_dump; bool was_true=false; long long last_fire_epoch=0; };` in `std::map<std::string, WhenState> when_state_by_id_`, pruned with the existing active-set loop.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_heartbeat_module.cpp` (the `Rig`, `at()`, `cron_store_path`, `write_store`, `local_epoch` helpers all exist; `add_record` is included; entries can be built + mutated before `add_entry`):

```cpp
namespace {
HeartbeatEntry when_entry(const std::string& name, const std::string& cond, bool notify = false,
                          long long cooldown = 60) {
  HeartbeatEntry e;
  e.name = name; e.prompt = "react"; e.notify = notify;
  e.when = cond; e.cooldown_s = cooldown;
  return e;
}
}  // namespace

TEST(Heartbeat, WhenChangesArmsThenFiresOncePerChange) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.add_entry(when_entry("w", "WATCHED changes", false, 0));   // cooldown 0: isolate edge logic
  r.bb.post("WATCHED", "v1", "test");
  r.mod.tick(at(0, 0));    // first observation ARMS, no fire
  EXPECT_EQ(turns, 0);
  r.mod.tick(at(1, 0));    // unchanged -> no fire
  EXPECT_EQ(turns, 0);
  r.bb.post("WATCHED", "v2", "test");
  r.mod.tick(at(2, 0));    // changed -> fire once
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(3, 0));    // re-armed on v2, unchanged -> no re-fire
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, WhenIsFiresOnEdgeAndRearmsOnFalse) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.add_entry(when_entry("w", "STATE is alarm", false, 0));
  r.bb.post("STATE", "ok", "test");
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 0);
  r.bb.post("STATE", "alarm", "test");
  r.mod.tick(at(1, 0));    // false->true edge -> fire
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(2, 0));    // still true -> no re-fire
  EXPECT_EQ(turns, 1);
  r.bb.post("STATE", "ok", "test");
  r.mod.tick(at(3, 0));    // re-armed
  r.bb.post("STATE", "alarm", "test");
  r.mod.tick(at(4, 0));    // new edge -> fire again
  EXPECT_EQ(turns, 2);
}

TEST(Heartbeat, WhenAlreadyTrueAtBootFiresOnce) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.bb.post("BUDGET", 0.9, "test");                        // already above before the first scan
  r.mod.add_entry(when_entry("w", "BUDGET above 0.8", false, 0));
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(1, 0));
  EXPECT_EQ(turns, 1);                                     // once
}

TEST(Heartbeat, WhenBusySkipDoesNotConsumeEdge) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.add_entry(when_entry("w", "WATCHED changes", false, 0));
  r.bb.post("WATCHED", "v1", "test");
  r.mod.tick(at(0, 0));                                    // arm
  r.bb.post("WATCHED", "v2", "test");
  {
    std::lock_guard<std::mutex> hold(r.gate.mu);           // human holds the gate
    r.mod.tick(at(1, 0));                                  // edge present but busy -> skipped, NOT consumed
  }
  EXPECT_EQ(turns, 0);
  r.mod.tick(at(2, 0));                                    // gate free -> the same edge fires
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, WhenCooldownAbsorbsFlap) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.add_entry(when_entry("w", "WATCHED changes", false, 3600));   // long cooldown
  r.bb.post("WATCHED", "v1", "test");
  r.mod.tick(at(0, 0));                                    // arm
  r.bb.post("WATCHED", "v2", "test");
  r.mod.tick(at(1, 0));                                    // fire (first)
  EXPECT_EQ(turns, 1);
  r.bb.post("WATCHED", "v3", "test");
  r.mod.tick(at(2, 0));                                    // edge INSIDE cooldown -> absorbed, no fire
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(3, 0));                                    // still absorbed (state advanced to v3)
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, WhenAbsentKeyNeverFires) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.add_entry(when_entry("w", "NEVER_POSTED is x", false, 0));
  r.mod.tick(at(0, 0));
  r.mod.tick(at(1, 0));
  EXPECT_EQ(turns, 0);
}

TEST(Heartbeat, DynamicWhenEdgeStateSurvivesReload) {
  Rig r;
  const std::string store = cron_store_path("dynwhen");
  CronTask t{"dw1", "watch", "when", "", 0, "react", false, 1};
  t.when = "WATCHED changes"; t.cooldown_s = 0;
  write_store(store, add_record(t) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.bb.post("WATCHED", "v1", "test");
  r.mod.tick(at(0, 0));    // reload builds the entry; arms on v1
  EXPECT_EQ(turns, 0);
  r.mod.tick(at(1, 0));    // RELOAD AGAIN (state must survive) — unchanged, no fire
  EXPECT_EQ(turns, 0);
  r.bb.post("WATCHED", "v2", "test");
  r.mod.tick(at(2, 0));    // change after reload -> exactly one fire
  EXPECT_EQ(turns, 1);
}
```

- [ ] **Step 2: Build — expect FAIL** (no `when` member on `HeartbeatEntry`).

- [ ] **Step 3: Implement.** Header — append to `HeartbeatEntry`:

```cpp
  std::string when;                  // reactive condition ("" = schedule-kind entry)
  long long cooldown_s = 60;         // min seconds between when-fires (flap absorber)
  bool when_armed = false;           // changes-kind: first observation recorded?
  std::string when_last_dump;        // changes-kind: last observed value (compact dump)
  bool when_was_true = false;        // holds-kind: previous evaluation (edge detect)
  long long when_last_fire_epoch = 0;
```

Add near `last_fired_by_id_`:

```cpp
  // Dynamic when-entries are rebuilt from the store each scan; their edge state lives here, keyed
  // by task id (the last_fired_by_id_ pattern), pruned to the active set on reload.
  struct WhenState { bool armed = false; std::string last_dump; bool was_true = false;
                     long long last_fire_epoch = 0; };
  std::map<std::string, WhenState> when_state_by_id_;
```

Declare `void maybe_fire_when_(HeartbeatEntry& e, long long now_epoch, bool dynamic);`.

`heartbeat.cpp` — `#include "hades/heartbeat/when.h"`. In `tick()`, route:

```cpp
  for (auto& e : entries_) {
    if (e.when.empty()) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/false);
    else                maybe_fire_when_(e, now_epoch, /*dynamic=*/false);
  }
  for (auto& e : dynamic_) {
    if (e.when.empty()) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/true);
    else                maybe_fire_when_(e, now_epoch, /*dynamic=*/true);
  }
```

Add the evaluator:

```cpp
void HeartbeatModule::maybe_fire_when_(HeartbeatEntry& e, long long now_epoch, bool dynamic) {
  // No minute-stamp gate here: edge detection + cooldown are the dedup for reactive entries.
  auto cond = parse_when(e.when);
  if (!cond) return;                                     // validated upstream; tolerate anyway
  auto entry = bb_->get(cond->key);
  const nlohmann::json* v = entry ? &entry->value : nullptr;

  bool edge = false;
  std::string new_dump = e.when_last_dump;
  bool new_true = e.when_was_true;
  if (cond->op == WhenCond::Op::Changes) {
    if (!v) { /* absent: hold state, nothing to compare */ }
    else {
      new_dump = v->dump();
      if (!e.when_armed) { e.when_armed = true; e.when_last_dump = new_dump; }   // arm, no fire
      else edge = (new_dump != e.when_last_dump);
    }
  } else {
    new_true = when_holds(*cond, v);
    edge = new_true && !e.when_was_true;
    if (!edge) e.when_was_true = new_true;               // re-arm on false; no-op while true
  }
  if (!edge) { sync_when_state_(e, dynamic); return; }

  // Cooldown: absorb the edge (advance state, no fire, no queueing).
  if (e.when_last_fire_epoch != 0 && now_epoch < e.when_last_fire_epoch + e.cooldown_s) {
    e.when_last_dump = new_dump;
    e.when_was_true = new_true;
    sync_when_state_(e, dynamic);
    return;
  }

  bool ran = false;
  try {
    ran = fire_(e);
  } catch (...) {
    bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
    ran = true;                                          // a throw mid-turn: don't re-fire the same edge
  }
  if (ran) {                                             // consume the edge ONLY if the turn was driven
    e.when_last_dump = new_dump;
    e.when_was_true = new_true;
    e.when_last_fire_epoch = now_epoch;
  }
  sync_when_state_(e, dynamic);                          // busy-skip: state untouched -> retry next tick
}
```

Add the state-sync helper + reload carry. Helper (private, declared in the header):

```cpp
void HeartbeatModule::sync_when_state_(const HeartbeatEntry& e, bool dynamic) {
  if (!dynamic || e.id.empty()) return;                  // static entries keep state in-place
  when_state_by_id_[e.id] = {e.when_armed, e.when_last_dump, e.when_was_true,
                             e.when_last_fire_epoch};
}
```

In `reload_dynamic_`'s rebuild loop, carry the when fields + state:

```cpp
    e.when = t.when; e.cooldown_s = t.cooldown_s;
    if (auto ws = when_state_by_id_.find(t.id); ws != when_state_by_id_.end()) {
      e.when_armed = ws->second.armed;
      e.when_last_dump = ws->second.last_dump;
      e.when_was_true = ws->second.was_true;
      e.when_last_fire_epoch = ws->second.last_fire_epoch;
    }
```

And extend the existing prune loop to also prune `when_state_by_id_` against `active_ids` (same idiom as `last_fired_by_id_`).

**Threading note for the implementer:** `bb_->get()` is thread-safe (locks the same mutex as `post()`), so reading latest values from the timer thread is race-free — this is the established pattern (`fire_` already posts from this thread).

- [ ] **Step 4: Build + test.** `-R Heartbeat` → all pass (every pre-existing + 7 new); FULL suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/heartbeat_module.h src/apps/heartbeat/heartbeat.cpp tests/test_heartbeat_module.cpp
git commit -m "feat: HeartbeatModule evaluates when-entries (edge-triggered, cooldown, reload-safe state)"
```

---

## Task 4: Wiring + docs + ship

**Files:** Modify `app/agent_wiring.cpp`; append to `tests/test_heartbeat_wiring.cpp`. Docs (controller-assisted): `manifests/dev.hades`, `docs/manifest-reference.md`, `prompts/soul.md`, `CLAUDE.md`.

**Interfaces:** a named `Heartbeat = <name>` block accepts `when` XOR `schedule` (both → `MalConfig`, neither → `MalConfig` with a message naming both options) + optional `cooldown_s`; `when` validated with `when_valid` → `MalConfig` on malformed.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_heartbeat_wiring.cpp` (read the file, reuse its manifest-builder idiom):

```cpp
TEST(HeartbeatWiring, WhenEntryAccepted) {
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\n"
      "Module = arbiter\nModule = heartbeat\n"
      "Heartbeat = watch\n{\n  when = BUDGET_SPENT_USD above 0.8\n  prompt = report it\n  cooldown_s = 300\n}\n");
  EXPECT_NO_THROW(build_agent(bb, m));
}

TEST(HeartbeatWiring, WhenAndScheduleTogetherThrow) {
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\n"
      "Module = arbiter\nModule = heartbeat\n"
      "Heartbeat = bad\n{\n  when = K changes\n  schedule = * * * * *\n  prompt = p\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(HeartbeatWiring, MalformedWhenThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\n"
      "Module = arbiter\nModule = heartbeat\n"
      "Heartbeat = bad\n{\n  when = KEY frobnicates\n  prompt = p\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
```

- [ ] **Step 2: Implement** in `app/agent_wiring.cpp` (the named-entry parse loop inside `if (a.heartbeat)`): add `#include "hades/heartbeat/when.h"`. Replace the schedule-required logic with the XOR:

```cpp
      const bool has_sched = b.kv.count("schedule") > 0;
      const bool has_when  = b.kv.count("when") > 0;
      if (has_sched == has_when)   // both or neither
        throw MalConfig("Heartbeat \"" + b.name + "\": exactly one of schedule|when is required");
      if (has_sched) {
        e.schedule = b.kv.at("schedule");
        if (!cron_valid(e.schedule))
          throw MalConfig("Heartbeat \"" + b.name + "\": invalid cron schedule: " + e.schedule);
      } else {
        e.when = b.kv.at("when");
        if (!when_valid(e.when))
          throw MalConfig("Heartbeat \"" + b.name + "\": invalid when condition: " + e.when);
        if (b.kv.count("cooldown_s")) e.cooldown_s = parse_ll(b.kv.at("cooldown_s"), e.cooldown_s);
        if (e.cooldown_s < 0) e.cooldown_s = 60;
      }
```

(Adapt to the loop's current shape — the prompt/prompt_file/notify handling stays as-is. `parse_ll` exists from self-scheduling; if scoped elsewhere, replicate its 3-line try/catch.)

- [ ] **Step 3: Build + test.** `-R HeartbeatWiring` → pass; FULL suite green.
- [ ] **Step 4: Docs (controller-assisted; implementer does NOT touch dev.hades/pi.hades/facts.md).**
  - `docs/manifest-reference.md` §15: the `when` key (grammar table, edge semantics, `cooldown_s`, the ~30s latency note, `schedule` XOR `when`), the schedule_task `when` kind in the self-scheduling subsection, the peer-origin veto note.
  - `prompts/soul.md` "## Scheduling your own work": one added sentence — a watch (`when`) is schedulable ("watch X and tell me when it changes").
  - `CLAUDE.md`: a `### Reactive when= trigger` shipped subsection + test-count bump + mark the v2-next line in the Heartbeat section DONE.
  - `manifests/dev.hades` (CONTROLLER ONLY — backup→checkout clean→edit→commit→restore): a commented `Heartbeat = watch { when = ... }` example next to the existing heartbeat examples.
- [ ] **Step 5: Full suite + TSan.** Both green.
- [ ] **Step 6: Commit** (wiring+tests by the implementer; docs by the controller as a separate commit).

```bash
git add app/agent_wiring.cpp tests/test_heartbeat_wiring.cpp
git commit -m "feat: wire when= heartbeat entries (XOR schedule, when_valid fail-loud, cooldown_s)"
```

---

## Verification (end-to-end)

1. Full suite + TSan in `nix develop`: baseline 529 + ~17 new, all green.
2. Manual live smoke (Vaios, optional): `Heartbeat = watch { when = PEER.pi0.card changes ... notify = true }` on the desktop agent → restart pi0 with a changed description → within ~5 min (discovery re-pull + tick) a Telegram notify arrives.
3. Agent-driven: "watch the budget and warn me above 0.5" → agent calls `schedule_task {when:"BUDGET_SPENT_USD above 0.5", notify:true}` → `list_tasks` shows it.

## Execution

Subagent-driven development: fresh implementer per task (sonnet T1/T2/T4; **opus T3** — the edge/skip/cooldown/reload semantics), per-task review (`cpp-reviewer`; opus for T3), fix loop for Critical/Important, final whole-branch review (opus), then finishing-a-development-branch (ff to `main`, no remote). Plan saved at `docs/superpowers/plans/2026-07-08-when-trigger.md`; commit before executing.
