# hades Self-Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the hades agent create, list, and cancel its own scheduled turns at runtime — recurring cron or one-shot ("in 10 min" / "at 09:00") — via three native tools backed by an append-only `.hades/cron.jsonl` store the `HeartbeatModule` re-reads live.

**Architecture:** Stateless subprocess tools append records to `.hades/cron.jsonl` (add/cancel/done, folded by id — the `memory.jsonl` pattern). `HeartbeatModule` loads+compacts it on boot and re-reads it every ~30s tick-scan; dynamic tasks coexist with static `Heartbeat` manifest entries. A tick is the SAME gated Arbiter self-turn a static heartbeat fires (`TURN_ORIGIN=heartbeat:<name>`). A `SelfScheduleGuard` objective (PeerLoopGuard sibling) blocks a heartbeat-origin turn from creating tasks unless `allow_self_schedule=true`; a human turn always may. Caps (`max_tasks`, `min_interval_s`) are enforced in the create tool.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, std::filesystem/std::chrono/ctime.

## Global Constraints

- **Every build/test runs inside `nix develop`:** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: current green suite (run `ctest` first to confirm — expected **473/473**). Each task adds its own tests and keeps the whole suite green.
- Branch `feat/self-scheduling` (created; spec committed `a0210fa`). Spec: `docs/superpowers/specs/2026-07-07-self-scheduling-design.md`.
- Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- **Store record schema is exact** (one JSON per line):
  - `{"op":"add","id":"<id>","name":"<n>","kind":"cron"|"once","schedule":<str|null>,"fire_epoch":<int|null>,"prompt":"<p>","notify":<bool>,"created":<int>}`
  - `{"op":"cancel","id":"<id>"}` · `{"op":"done","id":"<id>"}`
  - `schedule` is a string for `kind=="cron"` (else `null`); `fire_epoch` is a local epoch int for `kind=="once"` (else `null`).
- **`id` format is exactly** `t<created_epoch>-<hex4>` (e.g. `t1751900000-a3f9`), lowercase hex, 4 digits.
- **All times are machine-LOCAL epoch seconds** (`mktime`/`localtime_r`) — matches the machine-local cron convention. No timezone key in v1.
- **Tools do NOT link `hades_core`.** Each of the three tool binaries compiles `src/apps/heartbeat/cron_store.cpp` + `src/apps/heartbeat/cron.cpp` directly into itself and links only `nlohmann_json` (the `hades-save-skill` header-only precedent, extended to two shared .cpp).
- **Self-scheduling config lives in the UNNAMED `Heartbeat { }` block** (`Block.name == ""`); a NAMED `Heartbeat = <name> { }` block is a static entry as today. The entry-parse loop MUST `continue` on `b.name.empty()`.
- **Working-tree hygiene:** `manifests/dev.hades`, `manifests/pi.hades`, `memory/facts.md` carry the user's uncommitted LIVE config — **never stage them.** The Task 7 `dev.hades` edit is controller-handled (preserve the user's working copy). Never `git add -A`.
- Pump-thread handlers and tool `main` must **never throw** on adversarial input (try/catch / tolerant parse; degrade, don't crash). File headers: `// path — one-line purpose` + a short explanation block (house style).

---

## File Structure

```
include/hades/heartbeat/cron_store.h        T1  pure record model + fold/compact/serialize/parse_at/make_task_id
src/apps/heartbeat/cron_store.cpp           T1  impl (pure, tolerant, never throws)
tests/test_cron_store.cpp                   T1
tools/schedule_task_main.cpp                T2  hades-schedule-task binary (create)
tests/test_schedule_task_tool.cpp           T2
tools/list_tasks_main.cpp                   T3  hades-list-tasks binary (read)
tools/cancel_task_main.cpp                  T3  hades-cancel-task binary (delete)
tests/test_list_cancel_tools.cpp            T3
include/hades/module/heartbeat_module.h     T4  reload, dynamic_, one-shot, set_cron_store (modify)
src/apps/heartbeat/heartbeat.cpp            T4  reload_dynamic_/maybe_fire_/load_and_compact_ (modify)
tests/test_heartbeat_module.cpp             T4  (append: dynamic/one-shot/dedup/coexist/catch-up)
include/hades/objective/self_schedule_guard.h  T5  SelfScheduleGuard (new)
src/behaviors/standard_behaviors.cpp        T5  SelfScheduleGuard::veto (modify)
include/hades/objective/capability_policy.h T5  enum += SelfSchedule (modify)
src/behaviors/capability_policy.cpp         T5  capability_of + veto case (modify)
tests/test_self_schedule_guard.cpp          T5
app/agent_wiring.cpp                        T6  config split, argv append, set_cron_store, guard reg (modify)
tests/test_self_scheduling_wiring.cpp       T6
manifests/dev.hades, docs/manifest-reference.md, prompts/soul.md, CLAUDE.md, package.nix  T7  ship (modify)
CMakeLists.txt                              T1,T2,T3,T5,T6 (add sources/targets/tests)
```

---

## Task 1: `cron_store` pure library

**Files:** Create `include/hades/heartbeat/cron_store.h`, `src/apps/heartbeat/cron_store.cpp`, `tests/test_cron_store.cpp`. Modify `CMakeLists.txt`.

**Interfaces — Produces (all `namespace hades`):**
- `struct CronTask { std::string id, name, kind, schedule; long long fire_epoch=0; std::string prompt; bool notify=false; long long created=0; };`
- `std::vector<CronTask> fold_cron_store(const std::string& jsonl_text);` — sorted by (created,id); tolerant.
- `std::string compact_cron_store(const std::string& jsonl_text);` — active set re-serialized as add lines.
- `std::string add_record(const CronTask&);` / `std::string cancel_record(const std::string& id);` / `std::string done_record(const std::string& id);` — one JSON line, no trailing `\n`.
- `std::optional<long long> parse_at(const std::string& at, long long now_epoch);` — ISO or `HH:MM` next-occurrence, local; nullopt on failure.
- `std::string make_task_id(long long created_epoch, unsigned rand16);` — `t<epoch>-<hex4>`.

- [ ] **Step 1: Write the failing test** `tests/test_cron_store.cpp`:

```cpp
// tests/test_cron_store.cpp — pure cron.jsonl store: fold, compact, serialize, parse_at, id
#include <gtest/gtest.h>
#include <ctime>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using namespace hades;

TEST(CronStore, FoldAddCancelDone) {
  std::string s =
      add_record({"a1", "one", "cron", "*/5 * * * *", 0, "p1", true, 100}) + "\n" +
      add_record({"a2", "two", "once", "", 200, "p2", false, 101}) + "\n" +
      cancel_record("a1") + "\n";
  auto v = fold_cron_store(s);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a2");
  EXPECT_EQ(v[0].kind, "once");
  EXPECT_EQ(v[0].fire_epoch, 200);
  // a `done` tombstone also removes it
  auto v2 = fold_cron_store(s + done_record("a2") + "\n");
  EXPECT_TRUE(v2.empty());
}

TEST(CronStore, TolerantOfTornAndBlankLines) {
  std::string s = "\n{ this is not json\n" +
                  add_record({"a1", "n", "cron", "* * * * *", 0, "p", false, 5}) + "\n" +
                  "{\"op\":\"add\"}\n";   // no id -> skipped
  auto v = fold_cron_store(s);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a1");
}

TEST(CronStore, AddRecordJsonShape) {
  auto j = nlohmann::json::parse(add_record({"a1", "n", "cron", "0 9 * * *", 0, "hi", true, 7}));
  EXPECT_EQ(j["op"], "add");
  EXPECT_EQ(j["kind"], "cron");
  EXPECT_EQ(j["schedule"], "0 9 * * *");
  EXPECT_TRUE(j["fire_epoch"].is_null());     // cron -> null fire_epoch
  auto j2 = nlohmann::json::parse(add_record({"a2", "n", "once", "", 123, "hi", false, 7}));
  EXPECT_TRUE(j2["schedule"].is_null());      // once -> null schedule
  EXPECT_EQ(j2["fire_epoch"], 123);
}

TEST(CronStore, CompactDropsTombstoned) {
  std::string s = add_record({"a1", "n", "cron", "* * * * *", 0, "p", false, 1}) + "\n" +
                  add_record({"a2", "n", "once", "", 9, "p", false, 2}) + "\n" +
                  cancel_record("a1") + "\n";
  std::string c = compact_cron_store(s);
  auto v = fold_cron_store(c);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a2");
  EXPECT_EQ(c.find("cancel"), std::string::npos);   // no tombstone survives compaction
}

TEST(CronStore, ParseAtIsoAndHhmm) {
  // ISO absolute: round-trips through local mktime/localtime
  auto e = parse_at("2030-06-15T09:30", 0);
  ASSERT_TRUE(e.has_value());
  std::time_t tt = static_cast<std::time_t>(*e);
  std::tm lt{}; localtime_r(&tt, &lt);
  EXPECT_EQ(lt.tm_hour, 9);
  EXPECT_EQ(lt.tm_min, 30);
  EXPECT_EQ(lt.tm_year, 130);
  // bare HH:MM -> strictly in the future relative to now
  long long now = 1000000000;   // fixed reference
  auto h = parse_at("08:00", now);
  ASSERT_TRUE(h.has_value());
  EXPECT_GT(*h, now);
  EXPECT_LE(*h - now, 24 * 3600);   // within the next day
  EXPECT_FALSE(parse_at("not-a-time", now).has_value());
  EXPECT_FALSE(parse_at("25:99", now).has_value());
}

TEST(CronStore, MakeTaskIdFormat) {
  EXPECT_EQ(make_task_id(1751900000, 0xa3f9), "t1751900000-a3f9");
  EXPECT_EQ(make_task_id(5, 0x000f), "t5-000f");
}
```

- [ ] **Step 2: Add to CMake, build — expect FAIL (no header).** In `CMakeLists.txt`, after `target_sources(hades_core PRIVATE src/apps/heartbeat/cron.cpp)` (line ~26) add:

```cmake
target_sources(hades_core PRIVATE src/apps/heartbeat/cron_store.cpp)
```

and after `target_sources(hades_tests PRIVATE tests/test_cron.cpp)` (line ~56) add:

```cmake
target_sources(hades_tests PRIVATE tests/test_cron_store.cpp)
```

Run `nix develop --command cmake --build build` → compile error (missing header).

- [ ] **Step 3: Implement.** `include/hades/heartbeat/cron_store.h`:

```cpp
// include/hades/heartbeat/cron_store.h — pure task-store record model + fold/compact + time parse
//
// The .hades/cron.jsonl store is append-only with three op records: "add" (a task), "cancel" and
// "done" (tombstones). fold_cron_store replays a file's lines into the active task set (add inserts,
// cancel/done erase by id); compact_cron_store re-serializes that set as add-records. Pure string
// in/out (no file IO) so the three tools AND the module share one implementation and it is unit-
// testable. Times are machine-LOCAL epoch seconds (matches the machine-local cron convention).
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace hades {

struct CronTask {
  std::string id;              // "t<epoch>-<hex4>"
  std::string name;            // agent-chosen label
  std::string kind;            // "cron" | "once"
  std::string schedule;        // 5-field cron (kind=="cron"); "" otherwise
  long long   fire_epoch = 0;  // local epoch seconds (kind=="once"); 0 otherwise
  std::string prompt;          // the self-turn prompt
  bool        notify = false;
  long long   created = 0;     // local epoch seconds at creation
};

// Replay append-only jsonl into the active set. add -> insert by id; cancel/done -> erase. Tolerant:
// blank/corrupt/partial/id-less lines skipped. Sorted by (created, id) for a deterministic list.
std::vector<CronTask> fold_cron_store(const std::string& jsonl_text);

// The folded active set re-serialized as add-records (one per line, trailing '\n') = the compacted store.
std::string compact_cron_store(const std::string& jsonl_text);

// One record line (no trailing newline).
std::string add_record(const CronTask& t);
std::string cancel_record(const std::string& id);
std::string done_record(const std::string& id);

// Parse an absolute `at` to a local epoch. Accepts "YYYY-MM-DDTHH:MM[:SS]" and bare "HH:MM" (the next
// future occurrence vs now_epoch). nullopt on any unparseable input.
std::optional<long long> parse_at(const std::string& at, long long now_epoch);

// Task id from a creation epoch + 16 random bits: "t<epoch>-<hex4>".
std::string make_task_id(long long created_epoch, unsigned rand16);

}  // namespace hades
```

`src/apps/heartbeat/cron_store.cpp`:

```cpp
// src/apps/heartbeat/cron_store.cpp — cron.jsonl record model: fold, compact, serialize, time parse
#include "hades/heartbeat/cron_store.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
namespace hades {
namespace {
CronTask task_from_json(const nlohmann::json& j) {
  CronTask t;
  t.id       = j.value("id", "");
  t.name     = j.value("name", "");
  t.kind     = j.value("kind", "");
  if (j.contains("schedule") && j["schedule"].is_string()) t.schedule = j["schedule"].get<std::string>();
  if (j.contains("fire_epoch") && j["fire_epoch"].is_number_integer())
    t.fire_epoch = j["fire_epoch"].get<long long>();
  t.prompt   = j.value("prompt", "");
  t.notify   = j.value("notify", false);
  t.created  = j.value("created", 0LL);
  return t;
}
}  // namespace

std::vector<CronTask> fold_cron_store(const std::string& jsonl_text) {
  std::map<std::string, CronTask> active;   // id -> task; add inserts, cancel/done erase
  std::istringstream in(jsonl_text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;      // tolerant: skip a torn/partial line
    const std::string op = j.value("op", "");
    const std::string id = j.value("id", "");
    if (id.empty()) continue;
    if (op == "add") active[id] = task_from_json(j);
    else if (op == "cancel" || op == "done") active.erase(id);
  }
  std::vector<CronTask> out;
  out.reserve(active.size());
  for (auto& [id, t] : active) out.push_back(t);
  std::sort(out.begin(), out.end(), [](const CronTask& a, const CronTask& b) {
    return a.created != b.created ? a.created < b.created : a.id < b.id;
  });
  return out;
}

std::string add_record(const CronTask& t) {
  nlohmann::json j{{"op", "add"}, {"id", t.id}, {"name", t.name}, {"kind", t.kind},
                   {"schedule", t.kind == "cron" ? nlohmann::json(t.schedule) : nlohmann::json()},
                   {"fire_epoch", t.kind == "once" ? nlohmann::json(t.fire_epoch) : nlohmann::json()},
                   {"prompt", t.prompt}, {"notify", t.notify}, {"created", t.created}};
  return j.dump();
}
std::string cancel_record(const std::string& id) {
  return nlohmann::json{{"op", "cancel"}, {"id", id}}.dump();
}
std::string done_record(const std::string& id) {
  return nlohmann::json{{"op", "done"}, {"id", id}}.dump();
}

std::string compact_cron_store(const std::string& jsonl_text) {
  std::string out;
  for (const auto& t : fold_cron_store(jsonl_text)) out += add_record(t) + "\n";
  return out;
}

std::optional<long long> parse_at(const std::string& at, long long now_epoch) {
  int y, mo, d, h, mi, s = 0;
  if (std::sscanf(at.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) >= 5) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
    std::time_t e = std::mktime(&tm);
    if (e == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<long long>(e);
  }
  if (std::sscanf(at.c_str(), "%d:%d", &h, &mi) == 2 && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
    std::time_t now = static_cast<std::time_t>(now_epoch);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_hour = h; local.tm_min = mi; local.tm_sec = 0; local.tm_isdst = -1;
    std::time_t e = std::mktime(&local);
    if (e == static_cast<std::time_t>(-1)) return std::nullopt;
    if (static_cast<long long>(e) <= now_epoch) e += 24 * 3600;   // already passed -> tomorrow
    return static_cast<long long>(e);
  }
  return std::nullopt;
}

std::string make_task_id(long long created_epoch, unsigned rand16) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "t%lld-%04x", created_epoch, rand16 & 0xffffu);
  return buf;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R CronStore` → all pass. Then the full suite.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/heartbeat/cron_store.h src/apps/heartbeat/cron_store.cpp tests/test_cron_store.cpp CMakeLists.txt
git commit -m "feat: cron.jsonl store lib (fold/compact/serialize, parse_at, task id)"
```

---

## Task 2: `schedule_task` native tool

**Files:** Create `tools/schedule_task_main.cpp`, `tests/test_schedule_task_tool.cpp`. Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `cron_store.h` (`fold_cron_store`/`add_record`/`parse_at`/`make_task_id`) + `cron.h` (`cron_valid`), compiled into the binary.
- Produces: binary `hades-schedule-task`; argv `<store> <max_tasks> <min_interval_s>` (fallbacks `.hades/cron.jsonl`/`20`/`60`). Protocol `{"call":"describe"|"schedule_task","args":{name,prompt,notify?, one of schedule|in_minutes|at}}` → one JSON line. Success: `{"ok":true,"result":{"id","name","kind","when"}}`.

- [ ] **Step 1: Write the failing test** `tests/test_schedule_task_tool.cpp`:

```cpp
// tests/test_schedule_task_tool.cpp — drive hades-schedule-task over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_store(const char* tag) {
  const std::string p =
      (fs::path(::testing::TempDir()) / ("sched_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  fs::remove(p);
  return p;
}
static nlohmann::json call_sched(const std::string& store, const std::string& raw,
                                 const char* maxt = "20", const char* mini = "60") {
  ProcResult r = run_subprocess({SCHEDULE_TASK_BIN, store, maxt, mini}, raw, 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str();
}

TEST(ScheduleTaskTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SCHEDULE_TASK_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "schedule_task");
  auto req = j["result"]["schema"].value("required", nlohmann::json::array());
  for (const char* k : {"name", "prompt"})
    EXPECT_TRUE(std::find(req.begin(), req.end(), k) != req.end()) << k;
}

TEST(ScheduleTaskTool, CronTaskAppendsAddRecord) {
  const std::string store = fresh_store("cron");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"nightly","prompt":"summarize","notify":true,"schedule":"0 3 * * *"}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "cron");
  EXPECT_FALSE(j["result"].value("id", "").empty());
  const std::string body = slurp(store);
  EXPECT_NE(body.find("\"op\":\"add\""), std::string::npos);
  EXPECT_NE(body.find("0 3 * * *"), std::string::npos);
}

TEST(ScheduleTaskTool, InvalidCronRejected) {
  const std::string store = fresh_store("badcron");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"x","prompt":"p","schedule":"not a cron"}})");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(fs::exists(store));   // nothing written on rejection
}

TEST(ScheduleTaskTool, InMinutesBecomesOnce) {
  const std::string store = fresh_store("in");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"ping","in_minutes":10}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "once");
}

TEST(ScheduleTaskTool, InMinutesBelowFloorRejected) {
  const std::string store = fresh_store("floor");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"p","in_minutes":0}})", "20", "120");
  EXPECT_FALSE(j.value("ok", true));   // 0*60 < 120s floor
}

TEST(ScheduleTaskTool, AtAbsoluteBecomesOnce) {
  const std::string store = fresh_store("at");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"p","at":"2030-01-01T09:00"}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "once");
}

TEST(ScheduleTaskTool, ExactlyOneTimingRequired) {
  const std::string store = fresh_store("timing");
  // none
  EXPECT_FALSE(call_sched(store, R"({"call":"schedule_task","args":{"name":"x","prompt":"p"}})").value("ok", true));
  // two
  EXPECT_FALSE(call_sched(store,
      R"({"call":"schedule_task","args":{"name":"x","prompt":"p","in_minutes":5,"schedule":"* * * * *"}})").value("ok", true));
}

TEST(ScheduleTaskTool, MaxTasksCapRefuses) {
  const std::string store = fresh_store("cap");
  ASSERT_TRUE(call_sched(store, R"({"call":"schedule_task","args":{"name":"a","prompt":"p","schedule":"* * * * *"}})", "1", "60").value("ok", false));
  auto j = call_sched(store, R"({"call":"schedule_task","args":{"name":"b","prompt":"p","schedule":"* * * * *"}})", "1", "60");
  EXPECT_FALSE(j.value("ok", true));   // active count 1 >= cap 1
}

TEST(ScheduleTaskTool, MissingArgsAndNonStringFailClosed) {
  const std::string store = fresh_store("bad");
  for (const char* raw : {
       R"({"call":"schedule_task","args":{"prompt":"p","schedule":"* * * * *"}})",     // no name
       R"({"call":"schedule_task","args":{"name":"x","schedule":"* * * * *"}})",       // no prompt
       R"({"call":"schedule_task","args":{"name":7,"prompt":"p","schedule":"* * * * *"}})"}) {
    ProcResult r = run_subprocess({SCHEDULE_TASK_BIN, store}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** After the `hades-ask-agent` block (~line 113) add:

```cmake
add_executable(hades-schedule-task tools/schedule_task_main.cpp
               src/apps/heartbeat/cron_store.cpp src/apps/heartbeat/cron.cpp)
target_link_libraries(hades-schedule-task PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-schedule-task PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(hades_tests PRIVATE tests/test_schedule_task_tool.cpp)
target_compile_definitions(hades_tests PRIVATE SCHEDULE_TASK_BIN="$<TARGET_FILE:hades-schedule-task>")
add_dependencies(hades_tests hades-schedule-task)
```

- [ ] **Step 3: Implement** `tools/schedule_task_main.cpp`:

```cpp
// tools/schedule_task_main.cpp — bundled schedule_task native tool binary
//
// Creates a scheduled task by APPENDING an add-record to the cron store (argv[1], fallback
// .hades/cron.jsonl). One of schedule (5-field cron) | in_minutes (relative) | at (absolute local)
// is required; kind is "cron" or "once". Caps: argv[2] max_tasks (refuse when the active count is at
// the cap), argv[3] min_interval_s (one-shot delay floor). The store path + caps are fixed by wiring
// argv — never chosen by the LLM. Fail-closed: malformed/adversarial input returns ok:false, never
// throws. A task is a PROMPT to a future gated self-turn (never a raw command).
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron.h"         // cron_valid
#include "hades/heartbeat/cron_store.h"   // CronTask, fold_cron_store, add_record, parse_at, make_task_id

using nlohmann::json;
namespace fs = std::filesystem;

static std::string iso_local(long long epoch) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm lt{}; localtime_r(&t, &lt);
  char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &lt);
  return buf;
}

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/cron.jsonl";
  const long long max_tasks = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : 20;
  const long long min_interval_s = argc > 3 ? std::strtoll(argv[3], nullptr, 10) : 60;

  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);

  json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    json required = json::array({"name", "prompt"});
    out = {{"ok", true},
           {"result",
            {{"name", "schedule_task"},
             {"description",
              "Schedule one of YOUR OWN future turns. Provide name + prompt (the instruction your "
              "future self runs, gated as a normal turn — to run a command, say so in the prompt and "
              "you will call run_command then). Timing is exactly ONE of: schedule (5-field cron, "
              "recurring), in_minutes (run once, N minutes from now), at (run once, absolute "
              "'YYYY-MM-DDTHH:MM' or 'HH:MM' local). notify=true forwards the reply to the user. Use "
              "list_tasks/cancel_task to manage them."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"prompt", {{"type", "string"}}},
                 {"notify", {{"type", "boolean"}}},
                 {"schedule", {{"type", "string"}}},
                 {"in_minutes", {{"type", "number"}}},
                 {"at", {{"type", "string"}}}}},
               {"required", required}}}}}};
    std::cout << out.dump() << std::endl;
    return 0;
  }
  if (call != "schedule_task") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  auto str = [&](const char* k) {
    return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string{};
  };
  const std::string name = str("name");
  const std::string prompt = str("prompt");
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (name.empty() || prompt.empty()) return fail("missing arg: name and prompt required");

  const bool has_sched = args.contains("schedule") && args["schedule"].is_string();
  const bool has_in    = args.contains("in_minutes") && args["in_minutes"].is_number();
  const bool has_at    = args.contains("at") && args["at"].is_string();
  if (has_sched + has_in + has_at != 1)
    return fail("provide exactly one of: schedule, in_minutes, at");

  const long long now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  CronTask t;
  t.name = name; t.prompt = prompt; t.created = now;
  t.notify = args.contains("notify") && args["notify"].is_boolean() ? args["notify"].get<bool>() : false;
  std::string when;
  if (has_sched) {
    const std::string sched = args["schedule"].get<std::string>();
    if (!hades::cron_valid(sched)) return fail("invalid cron schedule: " + sched);
    t.kind = "cron"; t.schedule = sched; when = sched;
  } else if (has_in) {
    const double mins = args["in_minutes"].get<double>();
    const long long delay = static_cast<long long>(mins * 60);
    if (delay < min_interval_s) return fail("in_minutes below the min interval floor");
    t.kind = "once"; t.fire_epoch = now + delay; when = iso_local(t.fire_epoch);
  } else {
    auto e = hades::parse_at(args["at"].get<std::string>(), now);
    if (!e) return fail("unparseable at (want YYYY-MM-DDTHH:MM or HH:MM)");
    t.kind = "once"; t.fire_epoch = *e; when = iso_local(t.fire_epoch);
  }

  // Cap: refuse when the active set is already at max_tasks.
  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  if (static_cast<long long>(hades::fold_cron_store(body).size()) >= max_tasks)
    return fail("task cap reached (max_tasks=" + std::to_string(max_tasks) + ")");

  std::random_device rd;
  t.id = hades::make_task_id(now, rd());

  fs::path p(store);
  if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
  std::ofstream f(store, std::ios::app);
  if (!f) return fail("cannot append to store: " + store);
  f << hades::add_record(t) << "\n";

  std::cout << json{{"ok", true},
                    {"result", {{"id", t.id}, {"name", t.name}, {"kind", t.kind}, {"when", when}}}}.dump()
            << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `-R ScheduleTaskTool` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/schedule_task_main.cpp tests/test_schedule_task_tool.cpp CMakeLists.txt
git commit -m "feat: schedule_task native tool (cron/in_minutes/at, cap-gated, appends add-record)"
```

---

## Task 3: `list_tasks` + `cancel_task` native tools

**Files:** Create `tools/list_tasks_main.cpp`, `tools/cancel_task_main.cpp`, `tests/test_list_cancel_tools.cpp`. Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `cron_store.h` (`fold_cron_store`/`cancel_record`), compiled into each binary.
- Produces: `hades-list-tasks` (argv `<store>`; `{"call":"list_tasks"}` → `{"ok":true,"result":{"tasks":[...]}}`); `hades-cancel-task` (argv `<store>`; `{"call":"cancel_task","args":{"id"}}` → `{"ok":true,"result":{"cancelled":true,"id"}}` or `ok:false`).

- [ ] **Step 1: Write the failing test** `tests/test_list_cancel_tools.cpp`:

```cpp
// tests/test_list_cancel_tools.cpp — hades-list-tasks + hades-cancel-task over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string store_with(const char* tag, const std::string& contents) {
  const std::string p =
      (fs::path(::testing::TempDir()) / ("lc_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  std::ofstream f(p, std::ios::trunc); f << contents; return p;
}
static const char* kAdd =
    R"({"op":"add","id":"t1-aaaa","name":"nightly","kind":"cron","schedule":"0 3 * * *","fire_epoch":null,"prompt":"p","notify":true,"created":1})";

TEST(ListTasksTool, DescribeAndListActive) {
  const std::string store = store_with("list", std::string(kAdd) + "\n");
  ProcResult d = run_subprocess({LIST_TASKS_BIN}, R"({"call":"describe"})", 30.0);
  EXPECT_EQ(nlohmann::json::parse(d.out, nullptr, false)["result"].value("name", ""), "list_tasks");
  ProcResult r = run_subprocess({LIST_TASKS_BIN, store}, R"({"call":"list_tasks"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  auto tasks = j["result"]["tasks"];
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0].value("id", ""), "t1-aaaa");
  EXPECT_EQ(tasks[0].value("name", ""), "nightly");
}

TEST(ListTasksTool, EmptyOrMissingStoreYieldsEmpty) {
  ProcResult r = run_subprocess({LIST_TASKS_BIN, "/nonexistent/cron.jsonl"}, R"({"call":"list_tasks"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"]["tasks"].empty());
}

TEST(CancelTaskTool, CancelActiveAppendsTombstone) {
  const std::string store = store_with("cancel", std::string(kAdd) + "\n");
  ProcResult r = run_subprocess({CANCEL_TASK_BIN, store}, R"({"call":"cancel_task","args":{"id":"t1-aaaa"}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_TRUE(j["result"].value("cancelled", false));
  // re-list -> gone
  ProcResult r2 = run_subprocess({LIST_TASKS_BIN, store}, R"({"call":"list_tasks"})", 30.0);
  EXPECT_TRUE(nlohmann::json::parse(r2.out, nullptr, false)["result"]["tasks"].empty());
}

TEST(CancelTaskTool, CancelUnknownIdIsNotOk) {
  const std::string store = store_with("unknown", std::string(kAdd) + "\n");
  ProcResult r = run_subprocess({CANCEL_TASK_BIN, store}, R"({"call":"cancel_task","args":{"id":"ghost"}})", 30.0);
  EXPECT_FALSE(nlohmann::json::parse(r.out, nullptr, false).value("ok", true));
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** After the Task 2 block add:

```cmake
add_executable(hades-list-tasks tools/list_tasks_main.cpp src/apps/heartbeat/cron_store.cpp)
target_link_libraries(hades-list-tasks PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-list-tasks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(hades-cancel-task tools/cancel_task_main.cpp src/apps/heartbeat/cron_store.cpp)
target_link_libraries(hades-cancel-task PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-cancel-task PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(hades_tests PRIVATE tests/test_list_cancel_tools.cpp)
target_compile_definitions(hades_tests PRIVATE LIST_TASKS_BIN="$<TARGET_FILE:hades-list-tasks>")
target_compile_definitions(hades_tests PRIVATE CANCEL_TASK_BIN="$<TARGET_FILE:hades-cancel-task>")
add_dependencies(hades_tests hades-list-tasks hades-cancel-task)
```

- [ ] **Step 3: Implement** `tools/list_tasks_main.cpp`:

```cpp
// tools/list_tasks_main.cpp — bundled list_tasks native tool binary
//
// Lists the agent's OWN active dynamic tasks from the cron store (argv[1], fallback
// .hades/cron.jsonl). Static Heartbeat manifest entries are operator-owned and NOT in the store, so
// they are not listed. Read-only; fail-closed. `at` one-shots surface fire_epoch as a local ISO.
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using nlohmann::json;

static std::string iso_local(long long epoch) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm lt{}; localtime_r(&t, &lt);
  char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &lt);
  return buf;
}

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/cron.jsonl";
  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    std::cout << json{{"ok", true},
                      {"result",
                       {{"name", "list_tasks"},
                        {"description",
                         "List the scheduled tasks YOU created (id, name, kind, timing, prompt, "
                         "notify). Static operator-configured heartbeats are not shown. Use the id "
                         "with cancel_task to remove one."},
                        {"schema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}}}}
                     .dump()
              << std::endl;
    return 0;
  }
  if (call != "list_tasks") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  json tasks = json::array();
  for (const auto& t : hades::fold_cron_store(body)) {
    json e{{"id", t.id}, {"name", t.name}, {"kind", t.kind}, {"prompt", t.prompt}, {"notify", t.notify}};
    if (t.kind == "cron") e["schedule"] = t.schedule;
    else e["at"] = iso_local(t.fire_epoch);
    tasks.push_back(e);
  }
  std::cout << json{{"ok", true}, {"result", {{"tasks", tasks}}}}.dump() << std::endl;
  return 0;
}
```

`tools/cancel_task_main.cpp`:

```cpp
// tools/cancel_task_main.cpp — bundled cancel_task native tool binary
//
// Cancels one of the agent's OWN tasks by id: APPENDS a cancel tombstone to the cron store (argv[1],
// fallback .hades/cron.jsonl) if the id is currently active. Unknown/inactive id -> ok:false.
// Append-only (the module compacts on boot); fail-closed.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using nlohmann::json;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/cron.jsonl";
  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    std::cout << json{{"ok", true},
                      {"result",
                       {{"name", "cancel_task"},
                        {"description", "Cancel one of your scheduled tasks by its id (from list_tasks)."},
                        {"schema",
                         {{"type", "object"},
                          {"properties", {{"id", {{"type", "string"}}}}},
                          {"required", json::array({"id"})}}}}}}
                     .dump()
              << std::endl;
    return 0;
  }
  if (call != "cancel_task") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  const std::string id = args.contains("id") && args["id"].is_string() ? args["id"].get<std::string>() : "";
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (id.empty()) return fail("missing arg: id");

  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  bool active = false;
  for (const auto& t : hades::fold_cron_store(body)) if (t.id == id) { active = true; break; }
  if (!active) return fail("no active task with id: " + id);

  std::ofstream f(store, std::ios::app);
  if (!f) return fail("cannot append to store: " + store);
  f << hades::cancel_record(id) << "\n";
  std::cout << json{{"ok", true}, {"result", {{"cancelled", true}, {"id", id}}}}.dump() << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `-R "ListTasksTool|CancelTaskTool"` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/list_tasks_main.cpp tools/cancel_task_main.cpp tests/test_list_cancel_tools.cpp CMakeLists.txt
git commit -m "feat: list_tasks + cancel_task native tools (fold store, tombstone cancel)"
```

---

## Task 4: `HeartbeatModule` — reload dynamic tasks + one-shot

**Files:** Modify `include/hades/module/heartbeat_module.h`, `src/apps/heartbeat/heartbeat.cpp`. Append tests to `tests/test_heartbeat_module.cpp`.

**Interfaces:**
- Consumes: `cron_store.h` (`fold_cron_store`/`compact_cron_store`/`done_record`).
- Produces: `HeartbeatEntry` gains `std::string id; bool one_shot=false; long long fire_epoch=0;` (APPENDED — existing 5-field aggregate inits keep working). `HeartbeatModule::set_cron_store(std::string)`; `tick()` reloads the store, fires dynamic entries (cron + one-shot), dedups by id across reloads, and writes a `done` record after a one-shot fires.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_heartbeat_module.cpp`. Add these includes at the top of the file (next to the existing ones): `#include <filesystem>`, `#include <fstream>`, `#include "hades/heartbeat/cron_store.h"`. Then append:

```cpp
namespace {
std::string cron_store_path(const char* tag) {
  const std::string p = (std::filesystem::path(::testing::TempDir()) /
                         ("hbstore_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  std::filesystem::remove(p);
  return p;
}
void write_store(const std::string& path, const std::string& contents) {
  std::ofstream f(path, std::ios::trunc); f << contents;
}
long long local_epoch(const std::tm& t) { std::tm c = t; return static_cast<long long>(std::mktime(&c)); }
}  // namespace

TEST(Heartbeat, DynamicCronEntryFires) {
  Rig r;
  const std::string store = cron_store_path("dyncron");
  write_store(store, add_record({"d1", "watch", "cron", "*/10 * * * *", 0, "check X", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("TURN_ORIGIN", [&](const Entry& e) {
    if (e.value.is_string() && e.value.get<std::string>() == "heartbeat:watch") ++turns;
  });
  r.mod.tick(at(10, 4));   // */10 matches -> reloaded dynamic entry fires
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, OneShotFiresOnceThenDoneRecorded) {
  Rig r;
  const std::string store = cron_store_path("once");
  std::tm now = at(30, 9);
  long long past = local_epoch(now) - 60;   // due one minute ago
  write_store(store, add_record({"o1", "remind", "once", "", past, "ping", false, 5}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(now);                 // now_epoch >= fire_epoch -> fires
  EXPECT_EQ(turns, 1);
  // a done record was appended -> the task folds away
  std::ifstream f(store); std::stringstream s; s << f.rdbuf();
  EXPECT_TRUE(fold_cron_store(s.str()).empty());
  r.mod.tick(at(31, 9));           // next minute, reloads -> gone -> no re-fire
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, DynamicDedupSameMinuteAcrossReload) {
  Rig r;
  const std::string store = cron_store_path("dedup");
  write_store(store, add_record({"d1", "w", "cron", "* * * * *", 0, "c", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  r.mod.tick(at(0, 0));   // same minute, reloads again -> last_fired_by_id carries -> no double fire
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, StaticAndDynamicCoexist) {
  Rig r;
  const std::string store = cron_store_path("coexist");
  write_store(store, add_record({"d1", "dyn", "cron", "* * * * *", 0, "c", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  r.mod.add_entry({"stat", "* * * * *", "s", false, -1});   // static entry (5-field init still valid)
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 2);   // both fired
}

TEST(Heartbeat, OverdueOneShotCatchUpFires) {
  Rig r;
  const std::string store = cron_store_path("overdue");
  std::tm now = at(0, 12);
  long long long_ago = local_epoch(now) - 3 * 24 * 3600;   // 3 days overdue
  write_store(store, add_record({"o1", "late", "once", "", long_ago, "still do it", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(now);
  EXPECT_EQ(turns, 1);   // catch-up fires, not dropped
}
```

- [ ] **Step 2: Build — expect FAIL** (no `set_cron_store`, `add_record` not linked into module test — it is, `hades_core` has `cron_store.cpp`). Compile error on `set_cron_store`.

- [ ] **Step 3: Implement.** In `include/hades/module/heartbeat_module.h`: add `#include <map>` (near the other includes). Extend `HeartbeatEntry` (append the three fields):

```cpp
struct HeartbeatEntry {
  std::string name;
  std::string schedule;              // 5-field cron (cron kind)
  std::string prompt;                // resolved (inline or from prompt_file) at wiring
  bool notify = false;
  long long last_fired_minute = -1;  // per-entry minute-stamp dedup
  std::string id;                    // dynamic tasks only ("" for static manifest entries)
  bool one_shot = false;             // fire once at fire_epoch, then done
  long long fire_epoch = 0;          // local epoch seconds (one_shot)
};
```

Add the public setter (next to `set_turn_timeout_s`):

```cpp
  void set_cron_store(std::string path) { cron_store_ = std::move(path); }
```

Add private members + methods (next to `fire_`):

```cpp
  void maybe_fire_(HeartbeatEntry& e, const std::tm& now, long long minute, long long now_epoch,
                   bool dynamic);
  void reload_dynamic_();
  void load_and_compact_();
  std::string cron_store_;
  std::vector<HeartbeatEntry> dynamic_;
  std::map<std::string, long long> last_fired_by_id_;   // dedup survives a reload
```

In `src/apps/heartbeat/heartbeat.cpp`: add `#include <fstream>`, `#include <map>`, `#include <sstream>`, `#include "hades/heartbeat/cron_store.h"`. Replace the body of `tick()` (keep the minute stamp) with a static+dynamic pass, and add the three helpers. Replace the existing `tick()`:

```cpp
void HeartbeatModule::tick(const std::tm& now) {
  const long long minute =
      (static_cast<long long>(now.tm_year) * 100000000LL) + (now.tm_yday * 10000LL) +
      (now.tm_hour * 100LL) + now.tm_min;
  std::tm now_copy = now;
  const long long now_epoch = static_cast<long long>(std::mktime(&now_copy));
  if (!cron_store_.empty()) reload_dynamic_();          // pick up adds/cancels within one scan
  for (auto& e : entries_) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/false);
  for (auto& e : dynamic_) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/true);
}

void HeartbeatModule::maybe_fire_(HeartbeatEntry& e, const std::tm& now, long long minute,
                                  long long now_epoch, bool dynamic) {
  if (e.last_fired_minute == minute) return;             // already handled this minute (rescans)
  const bool match = e.one_shot ? (e.fire_epoch != 0 && now_epoch >= e.fire_epoch)
                                : cron_matches(e.schedule, now);
  if (!match) return;
  e.last_fired_minute = minute;                          // consume the minute (fire OR skip-if-busy)
  if (dynamic) last_fired_by_id_[e.id] = minute;         // carry the stamp across the next reload
  try {
    fire_(e);
  } catch (...) {
    bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
  }
  if (dynamic && e.one_shot && !cron_store_.empty()) {   // one-shot done -> tombstone, never re-fires
    std::ofstream f(cron_store_, std::ios::app);
    if (f) f << done_record(e.id) << "\n";
    last_fired_by_id_.erase(e.id);                       // gone after the next reload
  }
}

void HeartbeatModule::reload_dynamic_() {
  std::ifstream f(cron_store_);
  if (!f) { dynamic_.clear(); return; }
  std::stringstream ss; ss << f.rdbuf();
  std::vector<HeartbeatEntry> rebuilt;
  for (const auto& t : fold_cron_store(ss.str())) {
    HeartbeatEntry e;
    e.name = t.name; e.id = t.id; e.prompt = t.prompt; e.notify = t.notify;
    e.one_shot = (t.kind == "once");
    e.schedule = t.schedule; e.fire_epoch = t.fire_epoch;
    auto it = last_fired_by_id_.find(t.id);
    e.last_fired_minute = (it != last_fired_by_id_.end()) ? it->second : -1;
    rebuilt.push_back(std::move(e));
  }
  dynamic_ = std::move(rebuilt);
}

void HeartbeatModule::load_and_compact_() {
  if (cron_store_.empty()) return;
  std::ifstream f(cron_store_);
  if (!f) return;
  std::stringstream ss; ss << f.rdbuf();
  const std::string compacted = compact_cron_store(ss.str());
  std::ofstream out(cron_store_, std::ios::trunc);       // sole writer at boot
  if (out) out << compacted;
}
```

In `start()`, compact once before spawning the thread:

```cpp
void HeartbeatModule::start() {
  if (timer_thread_.joinable()) return;                  // idempotent
  load_and_compact_();                                   // drop tombstones/superseded on boot
  timer_thread_ = std::thread([this] {
    // ... unchanged body ...
```

- [ ] **Step 4: Build + test.** `-R Heartbeat` → all pass (new + existing); full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/heartbeat_module.h src/apps/heartbeat/heartbeat.cpp tests/test_heartbeat_module.cpp
git commit -m "feat: HeartbeatModule loads/reloads .hades/cron.jsonl — dynamic cron + one-shot tasks"
```

---

## Task 5: `SelfScheduleGuard` objective + `SelfSchedule` capability

**Files:** Create `include/hades/objective/self_schedule_guard.h`, `tests/test_self_schedule_guard.cpp`. Modify `src/behaviors/standard_behaviors.cpp`, `include/hades/objective/capability_policy.h`, `src/behaviors/capability_policy.cpp`, `CMakeLists.txt`.

**Interfaces:**
- Produces: `class SelfScheduleGuard : public Objective` (ctor takes `bool allow_self_schedule`); `Capability::SelfSchedule`; `capability_of("schedule_task"|"list_tasks"|"cancel_task") == Capability::SelfSchedule`; that capability → `allow()`.

- [ ] **Step 1: Write the failing test** `tests/test_self_schedule_guard.cpp`:

```cpp
// tests/test_self_schedule_guard.cpp — SelfScheduleGuard veto + SelfSchedule capability
#include <gtest/gtest.h>
#include "hades/blackboard.h"
#include "hades/objective/capability_policy.h"
#include "hades/objective/self_schedule_guard.h"
using namespace hades;

static Action sched(const char* tool) {
  Action a{Action::Kind::ToolCall};
  a.tool = tool;
  a.args = {{"name", "x"}, {"prompt", "p"}, {"schedule", "* * * * *"}};
  return a;
}

TEST(SelfScheduleGuard, HeartbeatOriginVetoedWhenDisallowed) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(false);
  EXPECT_TRUE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, HeartbeatOriginAllowedWhenEnabled) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(true);
  EXPECT_FALSE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, HumanOriginAlwaysAllowed) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "human", "chat");
  SelfScheduleGuard g(false);
  EXPECT_FALSE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, OnlyGuardsCreatePath) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(false);
  EXPECT_FALSE(g.veto(bb, sched("list_tasks")).vetoed);
  EXPECT_FALSE(g.veto(bb, sched("cancel_task")).vetoed);
}

TEST(CapabilityPolicy, SelfScheduleToolsAreAllowed) {
  EXPECT_EQ(CapabilityPolicy::capability_of("schedule_task"), Capability::SelfSchedule);
  EXPECT_EQ(CapabilityPolicy::capability_of("list_tasks"), Capability::SelfSchedule);
  EXPECT_EQ(CapabilityPolicy::capability_of("cancel_task"), Capability::SelfSchedule);
  CapabilityScope sc;
  CapabilityPolicy p(sc);
  Blackboard bb;
  EXPECT_FALSE(p.veto(bb, sched("schedule_task")).vetoed);   // allow (guard + tool caps do the gating)
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** After `target_sources(hades_tests PRIVATE tests/test_heartbeat_wiring.cpp)` (line ~181) add:

```cmake
target_sources(hades_tests PRIVATE tests/test_self_schedule_guard.cpp)
```

Build → compile error (no `self_schedule_guard.h`, no `Capability::SelfSchedule`).

- [ ] **Step 3: Implement.** `include/hades/objective/self_schedule_guard.h`:

```cpp
// include/hades/objective/self_schedule_guard.h — no self-scheduling from a heartbeat-driven turn
//
// Contains the runaway-recursion risk: a heartbeat tick (TURN_ORIGIN "heartbeat:<name>") may create
// new scheduled tasks ONLY when the operator set allow_self_schedule=true. A human-origin turn is
// never blocked (you set a goal, the agent schedules its own monitors). Guards ONLY the create path
// (schedule_task); list_tasks/cancel_task are always allowed. PeerLoopGuard sibling; auto-registered
// by wiring when heartbeat + the schedule_task tool are both present.
#pragma once
#include "hades/objective.h"
namespace hades {
class SelfScheduleGuard : public Objective {
 public:
  explicit SelfScheduleGuard(bool allow_self_schedule) : allow_(allow_self_schedule) {}
  std::string type() const override { return "self_schedule_guard"; }
  VetoResult veto(const Blackboard& bb, const Action& a) const override;

 private:
  bool allow_ = false;
};
}  // namespace hades
```

In `src/behaviors/standard_behaviors.cpp`: add `#include "hades/objective/self_schedule_guard.h"` and append:

```cpp
// ── no self-scheduling from a heartbeat-driven turn (recursion guard) ──────────────────────────
namespace hades {
VetoResult SelfScheduleGuard::veto(const Blackboard& bb, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall || a.tool != "schedule_task") return {};
  if (allow_) return {};                                  // operator opted in: ticks may self-schedule
  auto e = bb.get("TURN_ORIGIN");
  if (e && e->value.is_string() && e->value.get<std::string>().rfind("heartbeat:", 0) == 0)
    return {true, "a heartbeat-driven turn cannot self-schedule (allow_self_schedule=false)", false};
  return {};
}
}  // namespace hades
```

In `include/hades/objective/capability_policy.h`, extend the enum (append before `Unknown`):

```cpp
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SkillRead, SkillWrite, PeerAsk, GitRead, ExecScoped, SelfSchedule, Unknown };
```

In `src/behaviors/capability_policy.cpp`, in `capability_of` (before the final `return Capability::Unknown;`):

```cpp
  if (tool == "schedule_task" || tool == "list_tasks" || tool == "cancel_task")
                                                         return Capability::SelfSchedule;
```

In the `veto` switch (before `case Capability::Unknown:` / next to the SkillRead/SkillWrite case):

```cpp
    case Capability::SelfSchedule:
      // The agent's own task store: the store path + caps are fixed by wiring argv (never chosen by
      // the LLM), and SelfScheduleGuard already contains the heartbeat-origin recursion risk. Distinct
      // capability (SkillWrite precedent) so a future policy can confirm-gate scheduling with zero code.
      return allow();
```

- [ ] **Step 4: Build + test.** `-R "SelfScheduleGuard|CapabilityPolicy"` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/objective/self_schedule_guard.h src/behaviors/standard_behaviors.cpp include/hades/objective/capability_policy.h src/behaviors/capability_policy.cpp tests/test_self_schedule_guard.cpp CMakeLists.txt
git commit -m "feat: SelfScheduleGuard + SelfSchedule capability (heartbeat-origin create gate)"
```

---

## Task 6: Wiring — config split, tool argv, `set_cron_store`, guard registration

**Files:** Modify `app/agent_wiring.cpp`. Create `tests/test_self_scheduling_wiring.cpp`. Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `SelfScheduleGuard` (T5), `cron_store.h`, the heartbeat config (unnamed `Heartbeat {}` block), the three tool blocks.
- Produces: resolved `cron_store` (default `.hades/cron.jsonl`, `reject_ws`), `allow_self_schedule`/`max_tasks`/`min_interval_s` (defaults `false`/`20`/`60`); `schedule_task` argv gets `<cron_store> <max_tasks> <min_interval_s>`, `list_tasks`/`cancel_task` get `<cron_store>`; `a.heartbeat->set_cron_store(cron_store)`; `SelfScheduleGuard` registered when `a.heartbeat && schedule_task` rostered; the entry-parse loop skips the unnamed config block.

- [ ] **Step 1: Write the failing test** `tests/test_self_scheduling_wiring.cpp`:

```cpp
// tests/test_self_scheduling_wiring.cpp — manifest wiring for self-scheduling (real tools via ToolRunner)
// No llm module (the skills-wiring precedent): we post TOOL_REQUEST straight to the ToolRunner, which
// runs the REAL binaries synchronously on pump. The resolved argv (store + caps) is proven by the
// SIDE EFFECTS on cron_store — there is no argv accessor, and inventing one is the wrong pattern.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/heartbeat/cron_store.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string wire_store(const char* tag) {
  const std::string p = (fs::path(::testing::TempDir()) /
                         ("sw_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  fs::remove(p);
  return p;
}
// SCHEDULE_TASK_BIN / LIST_TASKS_BIN / CANCEL_TASK_BIN are the compiled target paths (compile-defs
// on hades_tests, added in Tasks 2-3), so the wired argv points at the real binaries.
static std::string manifest(const std::string& cron_store, const char* max_tasks = "20") {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = arbiter\n" +
         "Module = heartbeat\n" +
         "Tool = schedule_task { native = " + SCHEDULE_TASK_BIN + " }\n" +
         "Tool = list_tasks    { native = " + LIST_TASKS_BIN + " }\n" +
         "Tool = cancel_task   { native = " + CANCEL_TASK_BIN + " }\n" +
         "Heartbeat\n{\n  cron_store = " + cron_store + "\n  max_tasks = " + max_tasks +
         "\n  min_interval_s = 90\n}\n" +
         "Heartbeat = nightly\n{\n  schedule = 0 3 * * *\n  prompt = do it\n}\n";
}
static std::vector<CronTask> fold_file(const std::string& path) {
  std::ifstream f(path); std::stringstream s; s << f.rdbuf();
  return fold_cron_store(s.str());
}
static void post_schedule(Blackboard& bb, const char* id) {
  bb.post("TOOL_REQUEST",
          {{"id", id}, {"tool", "schedule_task"},
           {"args", {{"name", "n"}, {"prompt", "p"}, {"schedule", "* * * * *"}}}}, "arbiter");
  bb.pump();
}

TEST(SelfSchedulingWiring, ScheduleTaskWritesToWiredStore) {
  const std::string store = wire_store("store");
  Blackboard bb;
  Manifest m = parse_manifest(manifest(store));
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.tools, nullptr);
  post_schedule(bb, "c1");
  ASSERT_TRUE(fs::exists(store));           // argv carried the wired cron_store
  EXPECT_EQ(fold_file(store).size(), 1u);
}

TEST(SelfSchedulingWiring, MaxTasksCapReachesTheBinary) {
  const std::string store = wire_store("cap");
  Blackboard bb;
  Manifest m = parse_manifest(manifest(store, "1"));   // cap = 1
  Agent agent = build_agent(bb, m);
  post_schedule(bb, "a");   // ok
  post_schedule(bb, "b");   // cap 1 reached -> the binary refuses
  EXPECT_EQ(fold_file(store).size(), 1u);   // only the first add survived -> caps threaded via argv
}

TEST(SelfSchedulingWiring, UnnamedConfigBlockIsNotParsedAsEntry) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest(wire_store("cfg")));
  EXPECT_NO_THROW(build_agent(bb, m));   // the config block has no `schedule` -> must NOT MalConfig
}

TEST(SelfSchedulingWiring, WhitespaceCronStoreThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest("/tmp/has space.jsonl"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
```

- [ ] **Step 2: CMake + build — expect FAIL.** After the Task 5 test line add:

```cmake
target_sources(hades_tests PRIVATE tests/test_self_scheduling_wiring.cpp)
```

- [ ] **Step 3: Implement** in `app/agent_wiring.cpp`:

1. Add includes: `#include "hades/objective/self_schedule_guard.h"` and `#include "hades/heartbeat/cron_store.h"` (near the existing objective/heartbeat includes).

2. **Resolve the self-scheduling config** near the other path resolutions (after the `skills_dir` `reject_ws`, ~line 197). `heartbeat_blocks` is already a `wire_agent` parameter:

```cpp
  // Self-scheduling config lives in the UNNAMED `Heartbeat { }` block (a NAMED block is an entry).
  std::string cron_store = ".hades/cron.jsonl";
  bool allow_self_schedule = false;
  long long max_tasks = 20, min_interval_s = 60;
  auto parse_ll = [](const std::string& s, long long def) {
    try { return std::stoll(s); } catch (...) { return def; }
  };
  for (const auto& b : heartbeat_blocks) {
    if (!b.name.empty()) continue;                       // config block only
    if (b.kv.count("cron_store")) cron_store = b.kv.at("cron_store");
    if (b.kv.count("allow_self_schedule")) set_bool_on_string(b.kv.at("allow_self_schedule"), allow_self_schedule);
    if (b.kv.count("max_tasks")) max_tasks = parse_ll(b.kv.at("max_tasks"), max_tasks);
    if (b.kv.count("min_interval_s")) min_interval_s = parse_ll(b.kv.at("min_interval_s"), min_interval_s);
  }
  reject_ws(cron_store, "cron_store");
  bool has_schedule_task = false;
  for (const auto& t : tools) if (t.name == "schedule_task") has_schedule_task = true;
```

3. **Append the store + caps to the tool argv** in the `tools_resolved` loop (after the `ask_agent` branch, ~line 257):

```cpp
    else if (t.name == "schedule_task" && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + cron_store + " " + std::to_string(max_tasks) +
                       " " + std::to_string(min_interval_s);
    else if ((t.name == "list_tasks" || t.name == "cancel_task") && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + cron_store;
```

4. **Register the guard** next to the PeerLoopGuard registration (~line 321):

```cpp
    if (a.heartbeat && has_schedule_task)
      a.arbiter->add_objective(std::make_unique<SelfScheduleGuard>(allow_self_schedule));
```

5. **Skip the config block in the entry loop + set the store** (in the `if (a.heartbeat) {` section, ~line 407). Add `if (b.name.empty()) continue;` as the FIRST line of the `for (const auto& b : heartbeat_blocks)` loop, and after `a.heartbeat->set_turn_gate(a.gate.get());` add:

```cpp
    a.heartbeat->set_cron_store(cron_store);
```

- [ ] **Step 4: Build + test.** `-R SelfSchedulingWiring` → pass; **full suite** green.
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.cpp tests/test_self_scheduling_wiring.cpp CMakeLists.txt
git commit -m "feat: wire self-scheduling — Heartbeat config block, tool argv, set_cron_store, guard"
```

---

## Task 7: Ship — dev.hades, docs, soul.md, package.nix, lock

**Files:** Modify `manifests/dev.hades` (controller-handled — preserve the user's uncommitted working copy), `docs/manifest-reference.md`, `prompts/soul.md`, `CLAUDE.md`, `package.nix`.

**Note:** `manifests/dev.hades`/`pi.hades`/`memory/facts.md` carry the user's uncommitted LIVE config. The `dev.hades` edit adds ONLY a commented self-scheduling example; the controller performs it via backup→checkout-clean→edit→commit→restore so the user's working copy is preserved. Do NOT `git add` `pi.hades` or `memory/facts.md`.

- [ ] **Step 1: dev.hades commented example** (controller). Add after the heartbeat block region:

```
# --- Self-scheduling: let the agent create its own cron/one-shot tasks at runtime. ---
# The three tools are opt-in (roster them below). A HUMAN turn may schedule freely; a heartbeat
# TICK may schedule only if allow_self_schedule = true (recursion guard). Caps: max_tasks + the
# one-shot min_interval_s floor. Tasks persist in cron_store across restarts.
# Tool = schedule_task { native = ./build/hades-schedule-task }
# Tool = list_tasks    { native = ./build/hades-list-tasks }
# Tool = cancel_task   { native = ./build/hades-cancel-task }
# Heartbeat
# {
#   cron_store          = .hades/cron.jsonl
#   allow_self_schedule = false
#   max_tasks           = 20
#   min_interval_s      = 60
# }
```

- [ ] **Step 2: soul.md.** Append to the heartbeat/tools guidance a short paragraph:

```markdown
## Scheduling your own work

When a goal needs future or recurring action, schedule it: `schedule_task` creates one of your own
future turns — recurring (`schedule`, 5-field cron) or one-shot (`in_minutes`, or `at` a local
`YYYY-MM-DDTHH:MM`/`HH:MM`). The task is a PROMPT you write for your future self (to run a command,
say so in the prompt and you will call `run_command` then), `notify=true` forwards its reply to the
user. `list_tasks` shows what you have scheduled; `cancel_task` removes one by id. Prefer a one-shot
"check back" over asking the user to remind you.
```

- [ ] **Step 3: manifest-reference.md §15.** Extend the Heartbeat section with the unnamed-config-block keys (`cron_store`, `allow_self_schedule`, `max_tasks`, `min_interval_s`), the three tools, the `cron.jsonl` record schema, and the gotchas: config in the UNNAMED block (named = entry); a task is a prompt; human vs heartbeat origin gating; machine-local `at`; catch-up on boot.

- [ ] **Step 4: package.nix.** Add the three new targets to the `ninjaFlags`/built-binary list so the aarch64 static build ships them (grep the current list of `hades-*` targets and append `hades-schedule-task hades-list-tasks hades-cancel-task`).

- [ ] **Step 5: CLAUDE.md.** Add a `### Self-scheduling` subsection under Current state (3 tools + cron.jsonl + SelfScheduleGuard + caps + opt-in), bump the tool count (15→18) and test count, add the three targets to the targets line, add the gotchas (config in unnamed block; task=prompt; origin gate; machine-local at), and mark the NEXT self-scheduling item DONE.

- [ ] **Step 6: Full build + suite + TSan.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
# TSan run (per project process): reconfigure the tsan build dir if present and run the suite.
```

All green.

- [ ] **Step 7: Commit** (controller stages dev.hades carefully; never `pi.hades`/`facts.md`):

```bash
git add manifests/dev.hades docs/manifest-reference.md prompts/soul.md CLAUDE.md package.nix
git commit -m "feat: ship self-scheduling — dev.hades example, soul.md, manifest-reference, package.nix, docs"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: baseline + ~30 new, all green; TSan clean.
2. Manual live smoke (Vaios, needs `HADES_API_KEY`, roster the three tools + `Module = heartbeat`):
   ```
   user> every 2 minutes, check the time and tell me
     -> agent calls schedule_task {schedule:"*/2 * * * *", notify:true}; a task appears in .hades/cron.jsonl
   user> what have you scheduled?
     -> agent calls list_tasks, lists it
   (wait) -> a heartbeat tick fires the prompt every 2 min; notify reply arrives
   user> stop that
     -> agent calls cancel_task {id}; the tick stops next scan
   restart -> a pending one-shot survives; a recurring task resumes
   ```
3. One-shot: `remind me in 3 minutes to ...` → fires once after ~3 min, then a `done` record; no repeat.
4. Guardrail: with `allow_self_schedule=false`, a heartbeat tick that tries `schedule_task` is vetoed (Eventlog shows the veto); a human turn schedules fine.

## Execution

Subagent-driven development: fresh implementer per task (opus for T4/T6 — module reload concurrency + wiring; a cheaper tier for the mechanical tool tasks T1–T3, T5), per-task review (`cpp-reviewer`), fix loop for Critical/Important, final whole-branch review (opus), then finishing-a-development-branch (merge ff to `main`, no remote, never push). Save this plan is already at `docs/superpowers/plans/2026-07-07-self-scheduling.md`; commit it before executing.
