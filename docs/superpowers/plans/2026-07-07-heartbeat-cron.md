# Heartbeat / cron Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `HeartbeatModule` that fires cron-scheduled **self-turns** (no human) through the shared TurnGate — each a normal gated Arbiter turn — and, per a per-entry `notify` flag, forwards the reply to the user (Telegram) unless it's a `SILENT` sentinel.

**Architecture:** A timer thread wakes ~every 30s, evaluates each `Heartbeat` entry's cron against the machine-local minute, and (dedup once per minute) `try_lock`s the TurnGate → posts `TURN_ORIGIN=heartbeat:<name>` + `USER_MESSAGE` → `run_until` → notify-or-drop. A tick is a normal turn: capability_policy/avoid_destructive/stay_on_budget apply; confirm-band is **auto-denied** (no human). Spec: `docs/superpowers/specs/2026-07-07-heartbeat-cron-design.md`.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, std::thread, `<ctime>`.

## Global Constraints

- **Every build/test runs inside `nix develop`:** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. **Baseline: 450/450 green** before Task 1.
- Branch `feat/heartbeat` (created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- **A tick is a normal gated turn.** Do NOT bypass the Arbiter/objectives. The only additions are: confirm auto-deny (no human), skip-if-busy (`try_lock`), and the notify decision.
- **Threads started by `hades_main`, never `on_attach`** — tests spawn no timer thread (they call the `tick(now)` seam directly). The `build_agent` test overload must leave `agent.heartbeat == nullptr` and start nothing.
- **Teardown:** the timer thread is stop-flag + cv-notify + **joined in `~HeartbeatModule`**; `Agent::heartbeat` is declared LAST (destroyed FIRST) so its thread joins while the Telegram sink + Executor + Arbiter + ToolRunner + Blackboard are all alive. Do NOT reorder Agent members.
- Pump-thread / timer-thread handlers must **never throw** (try/catch; degrade). Cron helpers are pure and never throw.
- `SILENT` sentinel match is **case-sensitive exact** on the trimmed reply.
- Cron is **machine-local time** (`localtime_r`), minute resolution, **AND** across the 5 fields.
- **Do NOT stage** the user's uncommitted `manifests/dev.hades`/`manifests/pi.hades` live edits, `memory/facts.md`, or untracked `skills/`/`manifests/dev2.hades`/`build-tsan/`. Task 5's dev.hades edit touches only a **commented** example, committed split-clean (from the clean base, not the live edits — the prior stash-dance).
- File headers: `// path — one-line purpose` + a short explanation block (house style).
- **TSan lane at feature end** (Task 4 step): timer thread + the turn it drives + the Telegram send.

---

## File Structure

```
include/hades/heartbeat/cron.h          T1  pure cron_matches + cron_valid
src/apps/heartbeat/cron.cpp             T1
tests/test_cron.cpp                     T1  (new)
include/hades/module/heartbeat_module.h T2  HeartbeatModule (entries, tick seam, self-turn, timer)
src/apps/heartbeat/heartbeat.cpp        T2
tests/test_heartbeat_module.cpp         T2  (new)
src/apps/telegram/telegram.cpp          T3  subscribe NOTIFY_USER -> send_message to allow_users
include/hades/module/telegram_module.h  T3  (if a member/decl is needed)
tests/test_telegram_module.cpp          T3  (append)
app/agent_wiring.h / .cpp               T4  Agent.heartbeat, factory, Heartbeat parse + cron_valid + prompt_file
app/hades_main.cpp                      T4  agent.heartbeat->start() + heartbeat-only keep-alive
tests/test_heartbeat_wiring.cpp         T4  (new)
manifests/dev.hades, prompts/daily_summary.txt, docs/manifest-reference.md, CLAUDE.md  T5  ship
CMakeLists.txt                          T1,T2,T4  (add sources/tests)
```

---

## Task 1: Cron matcher (pure)

**Files:** Create `include/hades/heartbeat/cron.h`, `src/apps/heartbeat/cron.cpp`, `tests/test_cron.cpp`; Modify `CMakeLists.txt`.

**Interfaces — Produces (`namespace hades`):**
- `bool cron_matches(const std::string& expr, const std::tm& t)` — true if the 5-field cron matches `t` (machine-local). Tolerant: wrong field count / garbage → `false`.
- `bool cron_valid(const std::string& expr)` — true if `expr` is a parseable 5-field cron (for launch validation).

- [ ] **Step 1: Write the failing tests** `tests/test_cron.cpp`:

```cpp
// tests/test_cron.cpp — pure cron matcher (5-field, *,N,A-B,*/N,A,B; machine-local; AND fields)
#include <gtest/gtest.h>
#include <ctime>
#include "hades/heartbeat/cron.h"
using namespace hades;

// Build a std::tm for a given wall-clock; only the fields cron reads are set.
static std::tm mk(int min, int hour, int mday, int mon /*1-12*/, int wday /*0=Sun*/) {
  std::tm t{};
  t.tm_min = min; t.tm_hour = hour; t.tm_mday = mday; t.tm_mon = mon - 1; t.tm_wday = wday;
  return t;
}

TEST(Cron, Wildcard) {
  EXPECT_TRUE(cron_matches("* * * * *", mk(0, 0, 1, 1, 0)));
  EXPECT_TRUE(cron_matches("* * * * *", mk(37, 13, 25, 12, 3)));
}
TEST(Cron, ExactMinuteHour) {
  EXPECT_TRUE(cron_matches("0 6 * * *", mk(0, 6, 15, 3, 2)));
  EXPECT_FALSE(cron_matches("0 6 * * *", mk(1, 6, 15, 3, 2)));   // minute off
  EXPECT_FALSE(cron_matches("0 6 * * *", mk(0, 7, 15, 3, 2)));   // hour off
}
TEST(Cron, Step) {
  for (int m : {0, 10, 20, 30, 40, 50}) EXPECT_TRUE(cron_matches("*/10 * * * *", mk(m, 4, 1, 1, 1))) << m;
  for (int m : {1, 5, 11, 59})          EXPECT_FALSE(cron_matches("*/10 * * * *", mk(m, 4, 1, 1, 1))) << m;
}
TEST(Cron, RangeAndListAndDow) {
  EXPECT_TRUE(cron_matches("0 6 * * 1-5", mk(0, 6, 1, 1, 3)));    // Wed in 1-5
  EXPECT_FALSE(cron_matches("0 6 * * 1-5", mk(0, 6, 1, 1, 0)));   // Sun not in 1-5
  EXPECT_TRUE(cron_matches("0 9,17 * * *", mk(0, 17, 1, 1, 1)));  // list
  EXPECT_FALSE(cron_matches("0 9,17 * * *", mk(0, 12, 1, 1, 1)));
}
TEST(Cron, MonthAndDom) {
  EXPECT_TRUE(cron_matches("0 0 1 1 *", mk(0, 0, 1, 1, 4)));      // Jan 1 00:00
  EXPECT_FALSE(cron_matches("0 0 1 1 *", mk(0, 0, 2, 1, 5)));     // Jan 2
}
TEST(Cron, MalformedIsFalse) {
  EXPECT_FALSE(cron_matches("", mk(0, 0, 1, 1, 0)));
  EXPECT_FALSE(cron_matches("* * * *", mk(0, 0, 1, 1, 0)));        // 4 fields
  EXPECT_FALSE(cron_matches("* * * * * *", mk(0, 0, 1, 1, 0)));    // 6 fields
  EXPECT_FALSE(cron_matches("bogus * * * *", mk(0, 0, 1, 1, 0)));
  EXPECT_FALSE(cron_matches("*/0 * * * *", mk(0, 0, 1, 1, 0)));    // step 0
  EXPECT_FALSE(cron_matches("99 * * * *", mk(0, 0, 1, 1, 0)));     // out of range value never matches
}
TEST(Cron, Valid) {
  EXPECT_TRUE(cron_valid("*/10 * * * *"));
  EXPECT_TRUE(cron_valid("0 6 * * 1-5"));
  EXPECT_TRUE(cron_valid("0 9,17 * * *"));
  EXPECT_FALSE(cron_valid("* * * *"));       // 4 fields
  EXPECT_FALSE(cron_valid("bogus * * * *"));
  EXPECT_FALSE(cron_valid("*/0 * * * *"));
  EXPECT_FALSE(cron_valid("70 * * * *"));    // minute out of range
}
```

- [ ] **Step 2: Add to CMake, run — expect FAIL.** In `CMakeLists.txt`, near the other `src/apps/*` lines add:

```cmake
target_sources(hades_core PRIVATE src/apps/heartbeat/cron.cpp)
```

and near the test sources add:

```cmake
target_sources(hades_tests PRIVATE tests/test_cron.cpp)
```

Build → compile error (no header).

- [ ] **Step 3: Implement.** `include/hades/heartbeat/cron.h`:

```cpp
// include/hades/heartbeat/cron.h — pure 5-field cron matcher (min hour dom month dow)
//
// Evaluates a standard 5-field cron expression against a std::tm (machine-LOCAL time). Each field
// supports '*', N, A-B, A-B/N, '*/N', and comma lists (A,B,C). Fields are ANDed (the Vixie dom/dow
// OR quirk is intentionally NOT implemented). Tolerant: any parse failure or a field count != 5
// yields no-match (cron_matches) / invalid (cron_valid) — never throws. Minute resolution.
#pragma once
#include <ctime>
#include <string>
namespace hades {
bool cron_matches(const std::string& expr, const std::tm& t);
bool cron_valid(const std::string& expr);
}  // namespace hades
```

`src/apps/heartbeat/cron.cpp`:

```cpp
// src/apps/heartbeat/cron.cpp — 5-field cron parse + match (pure, tolerant, never throws)
#include "hades/heartbeat/cron.h"
#include <sstream>
#include <vector>
namespace hades {
namespace {

// Parse a decimal int from [b,e). Returns false on empty/non-digit/overflow.
bool parse_int(const std::string& s, std::size_t b, std::size_t e, int& out) {
  if (b >= e) return false;
  long v = 0;
  for (std::size_t i = b; i < e; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 100000) return false;
  }
  out = static_cast<int>(v);
  return true;
}

// Match one comma-less term (spec) for value v in [lo,hi]. Grammar: '*' | '*/N' | A | A-B | A-B/N.
// valid_out (optional) reports whether the term is well-formed even when it doesn't match v.
bool term_match(const std::string& spec, int v, int lo, int hi, bool& valid) {
  valid = false;
  if (spec.empty()) return false;
  // split off "/step"
  int step = 1;
  std::string base = spec;
  if (auto slash = spec.find('/'); slash != std::string::npos) {
    if (!parse_int(spec, slash + 1, spec.size(), step) || step <= 0) return false;
    base = spec.substr(0, slash);
  }
  int a = lo, b = hi;
  if (base == "*") {
    // a..b already lo..hi
  } else if (auto dash = base.find('-'); dash != std::string::npos) {
    if (!parse_int(base, 0, dash, a) || !parse_int(base, dash + 1, base.size(), b)) return false;
  } else {
    if (!parse_int(base, 0, base.size(), a)) return false;
    b = a;
  }
  if (a < lo || b > hi || a > b) return false;
  valid = true;                              // well-formed within range
  if (v < a || v > b) return false;
  return ((v - a) % step) == 0;
}

// Match a whole field (comma list) for value v in [lo,hi]. valid = every term well-formed.
bool field_match(const std::string& field, int v, int lo, int hi, bool& valid) {
  valid = true;
  bool any = false;
  std::stringstream ss(field);
  std::string term;
  bool saw_term = false;
  while (std::getline(ss, term, ',')) {
    saw_term = true;
    bool tv = false;
    if (term_match(term, v, lo, hi, tv)) any = true;
    if (!tv) valid = false;
  }
  if (!saw_term) valid = false;
  return any;
}

// Split into exactly 5 whitespace-separated fields. Returns false otherwise.
bool split5(const std::string& expr, std::vector<std::string>& out) {
  out.clear();
  std::stringstream ss(expr);
  std::string f;
  while (ss >> f) out.push_back(f);
  return out.size() == 5;
}

}  // namespace

bool cron_matches(const std::string& expr, const std::tm& t) {
  std::vector<std::string> f;
  if (!split5(expr, f)) return false;
  const int vals[5] = {t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon + 1, t.tm_wday};
  const int lo[5]   = {0, 0, 1, 1, 0};
  const int hi[5]   = {59, 23, 31, 12, 6};
  for (int i = 0; i < 5; ++i) {
    bool valid = false;
    if (!field_match(f[i], vals[i], lo[i], hi[i], valid)) return false;
  }
  return true;
}

bool cron_valid(const std::string& expr) {
  std::vector<std::string> f;
  if (!split5(expr, f)) return false;
  const int lo[5] = {0, 0, 1, 1, 0};
  const int hi[5] = {59, 23, 31, 12, 6};
  for (int i = 0; i < 5; ++i) {
    bool valid = false;
    field_match(f[i], lo[i], lo[i], hi[i], valid);   // value irrelevant; we only want well-formedness
    if (!valid) return false;
  }
  return true;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R Cron` → pass; then full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/heartbeat/cron.h src/apps/heartbeat/cron.cpp tests/test_cron.cpp CMakeLists.txt
git commit -m "feat: pure 5-field cron matcher (cron_matches/cron_valid)"
```

---

## Task 2: `HeartbeatModule` — entries, tick seam, self-turn, timer

**Files:** Create `include/hades/module/heartbeat_module.h`, `src/apps/heartbeat/heartbeat.cpp`, `tests/test_heartbeat_module.cpp`; Modify `CMakeLists.txt`.

**Interfaces — Produces:**
- `struct HeartbeatEntry { std::string name, schedule, prompt; bool notify = false; long long last_fired_minute = -1; };`
- `class HeartbeatModule : public Module` — `type()=="heartbeat"`, `add_entry(HeartbeatEntry)`, `set_turn_gate(TurnGate*)`, `set_turn_timeout_s(double)`, `on_attach(Blackboard&)` (captures reply + confirm auto-deny), `tick(const std::tm&)` (TEST SEAM — fire due entries), `start()` (spawn timer), dtor (stop+join).
- Bus keys posted: `TURN_ORIGIN`, `USER_MESSAGE`, `TURN_ABANDONED`, `CONFIRM_RESPONSE`, `NOTIFY_USER`, `HEARTBEAT_SKIPPED`, `HEARTBEAT_ERROR`.

- [ ] **Step 1: Write the failing tests** `tests/test_heartbeat_module.cpp`:

```cpp
// tests/test_heartbeat_module.cpp — HeartbeatModule: tick() fires gated self-turns, notify, guards
#include <gtest/gtest.h>
#include <ctime>
#include <mutex>
#include <string>
#include "hades/blackboard.h"
#include "hades/module/heartbeat_module.h"
#include "hades/turn_gate.h"
using namespace hades;

namespace {
std::tm at(int min, int hour) {   // a wall-clock minute; other fields are wildcarded in tests
  std::tm t{};
  t.tm_min = min; t.tm_hour = hour; t.tm_mday = 15; t.tm_mon = 5; t.tm_wday = 3;
  t.tm_year = 126; t.tm_yday = 165;
  return t;
}
// Rig: a HeartbeatModule wired to a bus with a scripted "agent" that echoes USER_MESSAGE ->
// ASSISTANT_MESSAGE, so run_until resolves without a real LLM.
struct Rig {
  Blackboard bb;
  TurnGate gate;
  HeartbeatModule mod;
  std::string reply = "report: all good";     // what the scripted agent answers
  Rig() {
    mod.set_turn_gate(&gate);
    mod.on_attach(bb);
    bb.subscribe("USER_MESSAGE",
                 [this](const Entry&) { bb.post("ASSISTANT_MESSAGE", reply, "test"); });
  }
};
}  // namespace

TEST(Heartbeat, FiresMatchingEntryOncePerMinute) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("TURN_ORIGIN", [&](const Entry& e) {
    if (e.value.is_string() && e.value.get<std::string>() == "heartbeat:mon") ++turns;
  });
  r.mod.add_entry({"mon", "*/10 * * * *", "check", false, -1});
  r.mod.tick(at(10, 4));   // matches */10
  r.mod.tick(at(10, 4));   // SAME minute -> dedup, no second fire
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(11, 4));   // non-matching minute
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(20, 4));   // next matching minute -> fires again
  EXPECT_EQ(turns, 2);
}

TEST(Heartbeat, NotifyFalseDropsReply) {
  Rig r;
  bool notified = false;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry&) { notified = true; });
  r.mod.add_entry({"task", "* * * * *", "do work", false, -1});
  r.mod.tick(at(0, 0));
  EXPECT_FALSE(notified);   // notify=false -> reply dropped
}

TEST(Heartbeat, NotifyTrueForwardsNonSilentReply) {
  Rig r;
  r.reply = "pi0 is DOWN";
  std::string got;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry& e) { got = e.value.value("text", ""); });
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  EXPECT_EQ(got, "pi0 is DOWN");
}

TEST(Heartbeat, NotifyTrueSilentSentinelIsDropped) {
  Rig r;
  r.reply = "SILENT";
  bool notified = false;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry&) { notified = true; });
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  EXPECT_FALSE(notified);
}

TEST(Heartbeat, SkipsWhenTurnGateBusy) {
  Rig r;
  bool fired = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { fired = true; });
  bool skipped = false;
  r.bb.subscribe("HEARTBEAT_SKIPPED", [&](const Entry&) { skipped = true; });
  std::lock_guard<std::mutex> hold(r.gate.mu);   // a "human turn" holds the gate
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  r.bb.pump();
  EXPECT_FALSE(fired);
  EXPECT_TRUE(skipped);
}

TEST(Heartbeat, ConfirmBandAutoDenied) {
  Blackboard bb;
  TurnGate gate;
  HeartbeatModule mod;
  mod.set_turn_gate(&gate);
  mod.on_attach(bb);
  nlohmann::json resp;
  // Scripted agent: on the tick, raise a confirm; when it's answered, finish the turn.
  bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "rm?"}}, "arbiter");
  });
  bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    resp = e.value;
    bb.post("ASSISTANT_MESSAGE", "[declined]", "arbiter");
  });
  mod.add_entry({"mon", "* * * * *", "risky", false, -1});
  mod.tick(at(0, 0));
  ASSERT_TRUE(resp.is_object());
  EXPECT_EQ(resp.value("id", ""), "c1");
  EXPECT_FALSE(resp.value("approved", true));
}

TEST(Heartbeat, NoEntryNoFire) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 0);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add:

```cmake
target_sources(hades_core PRIVATE src/apps/heartbeat/heartbeat.cpp)
target_sources(hades_tests PRIVATE tests/test_heartbeat_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/heartbeat_module.h`:

```cpp
// include/hades/module/heartbeat_module.h — cron self-trigger app (the autonomy leg)
//
// Owns a timer thread that wakes ~every 30s and fires a self-turn for each Heartbeat entry whose
// cron matches the machine-local minute (dedup once per minute). A tick is a NORMAL gated turn
// through the shared TurnGate: it try_locks the gate (skip-if-busy), posts TURN_ORIGIN=
// "heartbeat:<name>" + a USER_MESSAGE (the entry prompt), run_until()s the reply, then per the
// entry's notify flag forwards the reply to NOTIFY_USER (unless empty/"SILENT") or drops it. No
// human is present, so a confirm-band action is auto-denied. tick(std::tm) is the test seam (no
// clock/thread); start() spawns the thread (hades_main only); the dtor stop+joins it.
#pragma once
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "hades/module.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

struct HeartbeatEntry {
  std::string name;
  std::string schedule;              // 5-field cron
  std::string prompt;                // resolved (inline or from prompt_file) at wiring
  bool notify = false;
  long long last_fired_minute = -1;  // per-entry minute-stamp dedup
};

class HeartbeatModule : public Module {
 public:
  std::string type() const override { return "heartbeat"; }
  void on_attach(Blackboard& bb) override;
  void add_entry(HeartbeatEntry e) { entries_.push_back(std::move(e)); }
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  void tick(const std::tm& now);     // TEST SEAM: fire due entries for this wall-clock minute
  void start();                       // spawn the timer thread (hades_main; idempotent)
  void wait();                        // join the timer thread (heartbeat-only roster keep-alive)
  ~HeartbeatModule() override;        // stop + join

 private:
  void fire_(HeartbeatEntry& e);
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::vector<HeartbeatEntry> entries_;
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  double turn_timeout_override_s_ = 0.0;

  // Turn-capture state (timer thread only, under the gate while a tick runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  bool denied_confirm_ = false;
  std::string last_reply_;

  std::thread timer_thread_;
  std::mutex timer_mu_;
  std::condition_variable timer_cv_;
  bool timer_stop_ = false;
};
}  // namespace hades
```

`src/apps/heartbeat/heartbeat.cpp`:

```cpp
// src/apps/heartbeat/heartbeat.cpp — HeartbeatModule: cron timer -> gated self-turns -> notify/drop
#include "hades/module/heartbeat_module.h"
#include <algorithm>
#include <chrono>
#include "hades/blackboard.h"
#include "hades/heartbeat/cron.h"
#include "hades/timeouts.h"   // kDefaultTurnIdleTimeoutS
namespace hades {
namespace {
std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

double HeartbeatModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void HeartbeatModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Capture the reply of a tick we drive (my_turn_), symmetric to the front-ends.
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  // No human present -> auto-deny a confirm-band action inside our tick (mirror BridgeModule).
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    denied_confirm_ = true;
    auto id = e.value.find("id");
    bb_->post("CONFIRM_RESPONSE",
              {{"id", (id != e.value.end() && id->is_string()) ? id->get<std::string>() : ""},
               {"approved", false}},
              "heartbeat");
  });
}

void HeartbeatModule::tick(const std::tm& now) {
  // A stamp unique to this wall-clock minute (year+yday+hour+min); dedups double-wakes in a minute.
  const long long minute =
      (static_cast<long long>(now.tm_year) * 100000000LL) + (now.tm_yday * 10000LL) +
      (now.tm_hour * 100LL) + now.tm_min;
  for (auto& e : entries_) {
    if (e.last_fired_minute == minute) continue;      // already handled this minute
    if (!cron_matches(e.schedule, now)) continue;
    e.last_fired_minute = minute;                     // consume the minute (fire OR skip-if-busy)
    try {
      fire_(e);
    } catch (...) {
      bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
    }
  }
}

void HeartbeatModule::fire_(HeartbeatEntry& e) {
  std::unique_lock<std::mutex> lk(turn_mu_(), std::try_to_lock);
  if (!lk.owns_lock()) {                              // a human/peer turn holds the gate -> skip
    bb_->post("HEARTBEAT_SKIPPED", e.name, "heartbeat");
    return;
  }
  my_turn_ = true;
  struct Reset { bool& f; ~Reset() { f = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  denied_confirm_ = false;
  bb_->post("TURN_ORIGIN", "heartbeat:" + e.name, "heartbeat");
  bb_->post("USER_MESSAGE", "(scheduled heartbeat \"" + e.name + "\") " + e.prompt, "heartbeat");
  const bool done = bb_->run_until([this] { return got_reply_; }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "heartbeat");
    bb_->pump();
    bb_->post("HEARTBEAT_ERROR", e.name + " turn timed out", "heartbeat");
    return;
  }
  if (e.notify) {
    const std::string r = trim(last_reply_);
    if (!r.empty() && r != "SILENT") {
      bb_->post("NOTIFY_USER", {{"text", r}, {"from", "heartbeat:" + e.name}}, "heartbeat");
      bb_->pump();   // dispatch to the notify sink on THIS thread while we still hold the gate
    }
  }
}

void HeartbeatModule::start() {
  if (timer_thread_.joinable()) return;              // idempotent
  timer_thread_ = std::thread([this] {
    std::unique_lock<std::mutex> lk(timer_mu_);
    while (!timer_stop_) {
      if (timer_cv_.wait_for(lk, std::chrono::seconds(30), [this] { return timer_stop_; })) break;
      lk.unlock();
      try {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        tick(local);
      } catch (...) { /* never let a tick escape the thread */ }
      lk.lock();
    }
  });
}

void HeartbeatModule::wait() {
  if (timer_thread_.joinable()) timer_thread_.join();
}

HeartbeatModule::~HeartbeatModule() {
  {
    std::lock_guard<std::mutex> lk(timer_mu_);
    timer_stop_ = true;
  }
  timer_cv_.notify_all();
  if (timer_thread_.joinable()) timer_thread_.join();
}
}  // namespace hades
```

(Add `#include <cctype>` if `std::isspace` needs it.)

- [ ] **Step 4: Build + test.** `-R Heartbeat` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/heartbeat_module.h src/apps/heartbeat/heartbeat.cpp tests/test_heartbeat_module.cpp CMakeLists.txt
git commit -m "feat: HeartbeatModule — cron self-turns via TurnGate, notify/drop, confirm auto-deny"
```

---

## Task 3: Telegram `NOTIFY_USER` sink

**Files:** Modify `src/apps/telegram/telegram.cpp` (and `include/hades/module/telegram_module.h` only if needed); Test append `tests/test_telegram_module.cpp`.

**Interfaces:** `TelegramModule::on_attach` gains a `NOTIFY_USER` subscription → `api_->send_message(id, text)` for each `allow_users_` id. Best-effort, fail-soft. `NOTIFY_USER` value is `{text, from}` (object) — read `text`; tolerate a bare string too.

- [ ] **Step 1: Write the failing test** — append to `tests/test_telegram_module.cpp` (it already has a fake-`TelegramApi` rig; match it). If the existing fake records sent messages, assert against that; otherwise add a minimal capture. Example (adapt to the file's existing fake api name/fields):

```cpp
TEST(TelegramModule, NotifyUserSendsToAllowedUsers) {
  // Build a module with the file's fake api + allow_users {111, 222}; capture send_message calls.
  // (Use the same construction the other tests in this file use.)
  Blackboard bb;
  auto fake = std::make_unique<FakeApi>();          // the file's existing fake
  FakeApi* f = fake.get();
  TelegramModule m(std::move(fake));
  Block cfg;
  cfg.kv["allow_users"] = "111 222";
  cfg.kv["token_env"] = "TELEGRAM_TEST_TOKEN_UNUSED";   // avoid real token resolution if on_start needs it
  setenv("TELEGRAM_TEST_TOKEN_UNUSED", "x", 1);
  m.on_start(cfg, bb);
  m.on_attach(bb);
  bb.post("NOTIFY_USER", {{"text", "pi0 down"}, {"from", "heartbeat:mon"}}, "heartbeat");
  bb.pump();
  ASSERT_EQ(f->sent.size(), 2u);                    // one per allowed user (adapt field name)
  EXPECT_EQ(f->sent[0].second, "pi0 down");         // (chat_id, text) — adapt
  EXPECT_EQ(f->sent[0].first, 111);
  EXPECT_EQ(f->sent[1].first, 222);
}
```

(If the existing fake api does not record `send_message`, extend it minimally to push `{chat_id, text}` into a vector — the smallest change that lets this assert.)

- [ ] **Step 2: Run — expect FAIL** (no NOTIFY_USER handling).
- [ ] **Step 3: Implement.** In `TelegramModule::on_attach` (`src/apps/telegram/telegram.cpp`), after the existing subscriptions, add:

```cpp
  // Notify sink: a HeartbeatModule (or anything) posts NOTIFY_USER -> push to every allowed user.
  // Best-effort / fail-soft — a failed send must never crash the pump/timer thread.
  bb.subscribe("NOTIFY_USER", [this](const Entry& e) {
    if (!api_) return;
    std::string text;
    if (e.value.is_object())
      text = e.value.value("text", "");
    else if (e.value.is_string())
      text = e.value.get<std::string>();
    if (text.empty()) return;
    for (long long id : allow_users_) {
      try { api_->send_message(id, text); } catch (...) { /* fail-soft */ }
    }
  });
```

(Verify the member is named `allow_users_` and is a container of `long long`; adapt if the exact name/type differs. This handler is NOT `my_turn_`-gated — it fires whenever a notify is posted.)

- [ ] **Step 4: Build + test.** `-R TelegramModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add src/apps/telegram/telegram.cpp include/hades/module/telegram_module.h tests/test_telegram_module.cpp
git commit -m "feat: Telegram NOTIFY_USER sink — push heartbeat notifications to allow_users"
```

---

## Task 4: Wiring — `Agent.heartbeat`, factory, `Heartbeat` parse, start

**Files:** Modify `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`; Test `tests/test_heartbeat_wiring.cpp`; Modify `CMakeLists.txt`.

**Interfaces:** `Agent.heartbeat` (`std::unique_ptr<HeartbeatModule>`, LAST member); factory `"heartbeat"`; `Heartbeat` blocks parsed to entries (schedule via `cron_valid` else `MalConfig`; `prompt` or `prompt_file` — one required, `prompt_file` read at wiring else `MalConfig`; `notify` via `set_bool_on_string`); gate + idle-timeout injected; `hades_main` calls `start()` and keeps a heartbeat-only roster alive.

- [ ] **Step 1: Write the failing tests** `tests/test_heartbeat_wiring.cpp`:

```cpp
// tests/test_heartbeat_wiring.cpp — manifest-path wiring for the heartbeat module
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(HeartbeatWiring, ParsesEntriesInlineAndFile) {
  const std::string pf = std::string(::testing::TempDir()) + "/hb_prompt.txt";
  { std::ofstream f(pf); f << "summarize the day"; }
  const std::string mtext =
      "Session\n{\n  model = m\n}\n"
      "Module = arbiter\nModule = heartbeat\n"
      "Heartbeat = mon\n{\n  schedule = */10 * * * *\n  prompt = check pi0\n  notify = true\n}\n"
      "Heartbeat = daily\n{\n  schedule = 0 6 * * *\n  prompt_file = " + pf + "\n  notify = false\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.heartbeat, nullptr);
  // No timer thread is started by build_agent (only hades_main calls start()).
}

TEST(HeartbeatWiring, BadCronThrowsMalConfig) {
  const std::string mtext =
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = heartbeat\n"
      "Heartbeat = bad\n{\n  schedule = not a cron\n  prompt = x\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(HeartbeatWiring, MissingPromptThrowsMalConfig) {
  const std::string mtext =
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = heartbeat\n"
      "Heartbeat = np\n{\n  schedule = * * * * *\n  notify = true\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(HeartbeatWiring, NoHeartbeatRosterLeavesNull) {
  const std::string mtext = "Session\n{\n  model = m\n}\nModule = arbiter\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.heartbeat, nullptr);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add `target_sources(hades_tests PRIVATE tests/test_heartbeat_wiring.cpp)`.

- [ ] **Step 3: Implement.**

**`app/agent_wiring.h`:** add `#include "hades/module/heartbeat_module.h"` and, as the **LAST** member of `struct Agent` (after `telegram`):

```cpp
  // Cron self-trigger. Declared LAST => destroyed FIRST: its timer thread joins while the Telegram
  // notify sink + Executor + Arbiter + ToolRunner + Blackboard are all still alive (a tick drives a
  // full turn through them, and may notify via Telegram). Do NOT move above telegram.
  std::unique_ptr<HeartbeatModule> heartbeat;
```

**`app/agent_wiring.cpp`:**
1. Add `#include "hades/heartbeat/cron.h"`.
2. Register the factory with the others: `launcher.register_factory("heartbeat", []{ return std::make_unique<HeartbeatModule>(); });`
3. Take it: `a.heartbeat = take_as<HeartbeatModule>(launcher, "heartbeat");`
4. Extract the blocks near the other `m.of(...)` calls: `const auto heartbeat_blocks = m.of("Heartbeat");` and pass them into `wire_agent` (extend its signature with `const std::vector<Block>& heartbeat_blocks = {}` after the existing params; the test overload passes `{}`).
5. Inside `wire_agent`, after the bridge/telegram wiring, add:

```cpp
  // Heartbeat: parse each Heartbeat block into an entry (cron-validated; prompt inline or from file),
  // inject the shared gate + idle timeout, then attach. The timer thread is started by hades_main.
  if (a.heartbeat) {
    for (const auto& b : heartbeat_blocks) {
      HeartbeatEntry e;
      e.name = b.name;
      e.schedule = b.kv.count("schedule") ? b.kv.at("schedule") : "";
      if (!cron_valid(e.schedule))
        throw MalConfig("Heartbeat \"" + b.name + "\": invalid cron schedule: " + e.schedule);
      if (b.kv.count("prompt")) {
        e.prompt = b.kv.at("prompt");
      } else if (b.kv.count("prompt_file")) {
        std::ifstream pf(b.kv.at("prompt_file"));
        if (!pf) throw MalConfig("Heartbeat \"" + b.name + "\": cannot read prompt_file: " +
                                 b.kv.at("prompt_file"));
        std::stringstream ss; ss << pf.rdbuf();
        e.prompt = ss.str();
      } else {
        throw MalConfig("Heartbeat \"" + b.name + "\": requires prompt or prompt_file");
      }
      if (e.prompt.empty())
        throw MalConfig("Heartbeat \"" + b.name + "\": empty prompt");
      if (b.kv.count("notify")) set_bool_on_string(b.kv.at("notify"), e.notify);
      a.heartbeat->add_entry(std::move(e));
    }
    a.heartbeat->set_turn_gate(a.gate.get());
    a.heartbeat->set_turn_timeout_s(turn_idle_timeout_s);
    a.heartbeat->on_attach(bb);
  }
```

(Ensure `<fstream>`/`<sstream>` are included in `agent_wiring.cpp` — they are already used for other files.)

**`app/hades_main.cpp`:** after the bridge `start_listening()` block, add:

```cpp
    if (agent.heartbeat) agent.heartbeat->start();   // spawn the cron timer thread (after wiring)
```

And extend the keep-alive logic so a **heartbeat-only** roster (no chat/serve/telegram/bridge) still blocks instead of exiting: in the branch that currently blocks on telegram/bridge when there's no chat/serve, add heartbeat as a final fallback — e.g. `else if (agent.heartbeat) { std::cerr << "hades: heartbeat-only roster — running scheduled turns (Ctrl-C to exit)\n"; agent.heartbeat->wait(); }`. (Match the existing structure of that block.)

- [ ] **Step 4: Build + test.** `-R HeartbeatWiring` → pass; **full suite** green.
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp tests/test_heartbeat_wiring.cpp CMakeLists.txt
git commit -m "feat: wire heartbeat — Agent.heartbeat, Heartbeat block parse (cron/prompt_file), start"
```

- [ ] **Step 6: TSan lane.** Build + run the TSan config over the heartbeat + telegram + arbiter tests:
  `nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan -R 'Heartbeat|Telegram|Arbiter|Bridge|offload' --output-on-failure` → clean. Report the result. (If `build-tsan/` isn't configured, configure it with the project's ThreadSanitizer flags and note how.)

---

## Task 5: Ship — dev.hades, example prompt, docs

**Files:** Modify `manifests/dev.hades` (commented example only, split-clean), create `prompts/daily_summary.txt`, modify `docs/manifest-reference.md`, `CLAUDE.md`.

**Note:** `manifests/dev.hades` carries the user's uncommitted live edits. Touch ONLY a **commented** `Heartbeat` example; commit split-clean (from the clean committed base, not the working-tree edits). Do NOT stage `memory/facts.md`, `manifests/pi.hades`, `skills/`.

- [ ] **Step 1: `prompts/daily_summary.txt`** — a real example prompt file:

```
Summarize what happened in our conversations over the last day: decisions made, tasks
completed, and anything still open. Save the summary with save_memory so future sessions
recall it. Then reply exactly SILENT (this is a background task; do not notify).
```

- [ ] **Step 2: dev.hades** — add a commented example after the Telegram/Bridge section:

```
# --- Heartbeat: cron self-triggered turns. Module + >=1 Heartbeat block. notify=true forwards the
# reply to NOTIFY_USER (Telegram) unless the reply is SILENT; notify=false runs a silent task. ---
# Module = heartbeat
# Heartbeat = monitor
# {
#   schedule = */10 * * * *          # every 10 min (machine-local time)
#   prompt   = Check pi0 is reachable and healthy (ask it). Reply exactly SILENT if all is fine.
#   notify   = true
# }
# Heartbeat = daily
# {
#   schedule    = 0 6 * * *          # 06:00 daily
#   prompt_file = prompts/daily_summary.txt
#   notify      = false
# }
```

- [ ] **Step 3: `docs/manifest-reference.md`** — add a **§15 `Heartbeat` blocks** section: `Module = heartbeat`; the `schedule`/`prompt`/`prompt_file`/`notify` keys (table); cron subset (`* N A-B */N A,B`, AND fields, minute resolution, **machine-local time**); the `notify` → `NOTIFY_USER` → Telegram flow + the `SILENT` sentinel; the **inline-`prompt` `=` footgun** (use `prompt_file`); guardrails (confirm auto-deny, skip-if-busy, budget/caps apply, `TURN_ORIGIN=heartbeat:<name>` can delegate). Add a one-line note under §10 Telegram that it subscribes `NOTIFY_USER` (sends to `allow_users`). Update the §2 roster table with the `heartbeat` module row. Note in Appendix (env/bus) the new `TURN_ORIGIN=heartbeat:<name>` value and `NOTIFY_USER`/`HEARTBEAT_SKIPPED`/`HEARTBEAT_ERROR` bus keys.

- [ ] **Step 4: `CLAUDE.md`** — add a `### Heartbeat / cron (self-triggered turns) — shipped 2026-07-07` subsection under Current state (timer → gated self-turn, cron subset + machine-local, per-entry `notify` + `SILENT` + Telegram sink, confirm auto-deny + skip-if-busy, `TURN_ORIGIN=heartbeat:<name>`, Eventlog = activity log); bump the test count; mark **NEXT direction 2 (heartbeat/cron) SHIPPED**; add the gotchas (inline-`prompt` `=` footgun → `prompt_file`; machine-local TZ; `notify=true` needs Telegram rostered; the reactive `when=` consumer is the v2 next step). Update the targets/roster line if it enumerates modules.

- [ ] **Step 5: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green.

- [ ] **Step 6: Commit (split-clean for dev.hades).**

```bash
git add prompts/daily_summary.txt docs/manifest-reference.md CLAUDE.md
# dev.hades: commit ONLY the commented-example change from the clean base (protect live edits) —
# controller applies the established stash-dance; do NOT `git add manifests/dev.hades` blindly.
git commit -m "feat: ship heartbeat — dev.hades example + daily_summary prompt + docs"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 450 baseline + ~20 new, all green. TSan clean (Task 4 step 6).
2. Manual live smoke (Vaios): uncomment the `Heartbeat = monitor` block (schedule `* * * * *` for a fast test) + roster Telegram; boot → within a minute a self-turn fires (`hades-scope session.log TURN_ORIGIN` shows `heartbeat:monitor`); if the reply isn't `SILENT`, a Telegram message arrives. A `notify=false` entry runs silently (tool actions in the Eventlog, no message). While you're mid-conversation, a due tick is skipped (`HEARTBEAT_SKIPPED`), not queued.
3. Guardrail check: a heartbeat entry whose prompt asks for a confirm-band action (e.g. `shell`) → auto-denied (no Telegram approval prompt), noted in the reply.

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus), per-task review, TSan lane after Task 4, final whole-branch review, then finishing-a-development-branch (merge ff to `main` — no remote, never push). Baseline 450/450 before Task 1.
