# hades Telegram Front-End Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Talk to the hades agent from Telegram — a per-app front-end module (`Module = telegram`) that long-polls the Bot API, drives turns through a shared `TurnGate` alongside the REPL and web UI, and confirm-gates via inline keyboard.

**Architecture:** A `TurnGate` (one mutex, Agent's FIRST member) serializes whole turns across all front-ends — the single-threaded-pump invariant now rests on the gate instead of "one front-end per process". A pure parse/builder lib + a `TelegramApi` seam (cpr real impl, scripted fake in tests) keep the module network-free in tests. `TelegramModule` owns a poll thread (explicit `start_polling()`, stop+join in dtor), discards the startup backlog, allowlists numeric user IDs fail-fast, splits replies at 4096, and handles confirms via `callback_query`. Spec: `docs/superpowers/specs/2026-07-02-telegram-front-end-design.md` (committed on `feat/telegram`).

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, cpr (already a dep), nlohmann_json, GoogleTest, std::thread.

## Global Constraints

- **Every build/test command runs inside `nix develop`** from the repo root: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Build dir already configured — do NOT reconfigure. Baseline: **284/284 green** before Task 1.
- Branch `feat/telegram` (exists; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- Bot token via **env var only** (`token_env`, default exactly `TELEGRAM_BOT_TOKEN`); never in the manifest; **redacted in the Eventlog**.
- `allow_users` (whitespace-separated numeric Telegram user IDs) is **REQUIRED** when `Module = telegram` is rostered → `MalConfig` if absent/empty/non-numeric. Non-allowed senders **silently ignored** (no reply).
- Telegram message hard limit **4096 chars** → split. Long-poll `poll_timeout_s` default **50**.
- Callback data format exactly `approve:<confirm_id>` / `deny:<confirm_id>`.
- **Turn-owner guard:** every front-end handles CONFIRM_REQUEST/reply-capture ONLY for turns it initiated (`my_turn_` flag); pump-thread handlers NEVER throw (type-guards, house rule).
- `Agent`'s `executor` member MUST remain LAST (teardown-critical); the new `TurnGate gate` member MUST be FIRST (destroyed last, outlives modules).
- Do NOT touch `memory/facts.md` or the untracked `skills/` dir. Never push (no remote).
- One key=value per manifest line (packed lines fail `enforce_manifest`).

---

## File Structure

```
include/hades/turn_gate.h                 T1  shared whole-turn serializer (header-only)
include/hades/module/http_server_module.h T1  gate adoption (replace private mu_)
src/module/http_server_module.cpp         T1
tests/test_turn_gate.cpp                  T1
include/hades/module/chat_module.h        T2  gate + my_turn_ confirm guard
src/module/chat_module.cpp                T2
include/hades/telegram/parse.h            T3  TgUpdate, parse_updates, split_message, body builders
src/telegram/parse.cpp                    T3
tests/test_telegram_parse.cpp             T3
include/hades/telegram/api.h              T4  TelegramApi interface
include/hades/telegram/cpr_telegram_api.h T4  real impl (cpr)
src/telegram/cpr_telegram_api.cpp         T4
include/hades/module/telegram_module.h    T5  TelegramModule
src/module/telegram_module.cpp            T5
tests/test_telegram_module.cpp            T5
app/agent_wiring.{h,cpp}                  T6  Agent.gate + Agent.telegram + factory + wiring
app/hades_main.cpp                        T6  token redaction, start_polling, telegram-only wait
tests/test_telegram_wiring.cpp            T6
manifests/dev.hades, .gitignore, CLAUDE.md T6 ship
CMakeLists.txt                            T1,T3,T4,T5,T6
```

---

## Task 1: `TurnGate` + HttpServerModule adoption

**Files:**
- Create: `include/hades/turn_gate.h`
- Modify: `include/hades/module/http_server_module.h` (replace `std::mutex mu_`), `src/module/http_server_module.cpp` (lock sites in `handle_message`/`handle_confirm`)
- Test: `tests/test_turn_gate.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `struct hades::TurnGate { std::mutex mu; };`; `HttpServerModule::set_turn_gate(TurnGate*)`. Null gate → module-local fallback (existing tests byte-identical).

- [ ] **Step 1: Write the failing test** `tests/test_turn_gate.cpp`:

```cpp
// tests/test_turn_gate.cpp — shared TurnGate serializes whole turns across front-end threads
#include <gtest/gtest.h>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "hades/blackboard.h"
#include "hades/module/http_server_module.h"
#include "hades/turn_gate.h"
using namespace hades;

TEST(TurnGate, TurnWaitsWhileAnotherFrontEndHoldsTheGate) {
  Blackboard bb;
  HttpServerModule m;
  TurnGate gate;
  m.set_turn_gate(&gate);
  m.on_attach(bb);
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
  });
  std::vector<std::string> order;   // ordering established by the gate's acquire/release
  std::unique_lock<std::mutex> hold(gate.mu);          // simulate another front-end mid-turn
  std::thread t([&] {
    auto r = m.handle_message("late");                 // must block on the shared gate
    order.push_back("turn:" + r.value("reply", ""));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  order.push_back("released");
  hold.unlock();
  t.join();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "released");                     // the turn could not run while held
  EXPECT_EQ(order[1], "turn:echo:late");
}

TEST(TurnGate, NullGateFallsBackToLocalSerialization) {
  Blackboard bb;
  HttpServerModule m;                                  // no set_turn_gate call
  m.on_attach(bb);
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
  });
  auto r = m.handle_message("hi");
  EXPECT_EQ(r.value("reply", ""), "echo:hi");          // pre-gate behavior intact
}
```

- [ ] **Step 2: CMake + run — expect FAIL** (no `turn_gate.h` / `set_turn_gate`). Add after the `test_serve.cpp` line in `CMakeLists.txt`:

```cmake
target_sources(hades_tests PRIVATE tests/test_turn_gate.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/turn_gate.h`:

```cpp
// include/hades/turn_gate.h — shared whole-turn serializer for concurrent front-ends
//
// One agent = one bus = ONE turn at a time. Each front-end (REPL, HTTP, Telegram) locks this
// gate around its post(USER_MESSAGE / CONFIRM_RESPONSE) -> run_until(...) sequence, so exactly
// one thread pumps the Blackboard at any moment — the single-threaded-dispatch invariant now
// rests on this gate instead of "one front-end per process". Owned by Agent as its FIRST member
// (destroyed LAST — outlives every module holding a pointer). Idle front-ends (REPL blocked on
// stdin, HTTP awaiting a request, Telegram long-polling) hold NOTHING — other surfaces proceed.
#pragma once
#include <mutex>
namespace hades {
struct TurnGate {
  std::mutex mu;
};
}  // namespace hades
```

In `include/hades/module/http_server_module.h`: add `#include "hades/turn_gate.h"`; add public

```cpp
  // Shared whole-turn serializer (see turn_gate.h). Null (tests / single front-end) -> the
  // module-local fallback gate, byte-identical to the old private mu_ behavior.
  void set_turn_gate(TurnGate* g) { gate_ = g; }
```

and replace the private `std::mutex mu_;` with

```cpp
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  TurnGate* gate_ = nullptr;   // shared across front-ends when set (wiring)
  TurnGate  local_gate_;       // fallback so an un-wired module still serializes its own turns
```

In `src/module/http_server_module.cpp`: `handle_message` and `handle_confirm` change
`std::lock_guard<std::mutex> lk(mu_);` → `std::lock_guard<std::mutex> lk(turn_mu_());`.

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R TurnGate` → 2/2; FULL suite green (284 + 2).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/turn_gate.h include/hades/module/http_server_module.h src/module/http_server_module.cpp tests/test_turn_gate.cpp CMakeLists.txt
git commit -m "feat: TurnGate — shared whole-turn serializer; HttpServerModule adopts it"
```

---

## Task 2: ChatModule gate adoption + turn-owner confirm guard

**Files:**
- Modify: `include/hades/module/chat_module.h`, `src/module/chat_module.cpp`

**Interfaces:**
- Consumes: `TurnGate` (T1).
- Produces: `ChatModule::set_turn_gate(TurnGate*)`; `my_turn_` guard semantics: the CONFIRM_REQUEST handler reads y/N **only** when `in_` is set AND `my_turn_` is true (a foreign front-end's confirm must never read this REPL's stdin — the handler runs on the *pumping* thread, which for a Telegram-driven turn is the Telegram poll thread).

- [ ] **Step 1: Modify header.** In `include/hades/module/chat_module.h`: add `#include "hades/turn_gate.h"`; public

```cpp
  // Shared whole-turn serializer (null -> module-local fallback; single-front-end behavior).
  void set_turn_gate(TurnGate* g) { gate_ = g; }
```

private members

```cpp
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  TurnGate* gate_ = nullptr;
  TurnGate  local_gate_;
  // True only while THIS module holds the gate and is driving its own turn. The
  // CONFIRM_REQUEST handler must not read y/N from stdin for another front-end's turn
  // (it runs on whichever thread is pumping — for a Telegram turn, the poll thread).
  bool my_turn_ = false;
```

(add `#include <mutex>` if not present).

- [ ] **Step 2: Modify `src/module/chat_module.cpp`.** In `on_attach`, change the CONFIRM_REQUEST handler's first line from `if (!in_) return;` to:

```cpp
    if (!in_ || !my_turn_) return;  // answer only THIS repl's own gated turn (see header note)
```

In **both** loops (`run_repl` getline loop and `run_repl_readline`), wrap the turn drive in the gate — replace

```cpp
    turn_done_ = false;
    bb_->post("USER_MESSAGE", line, "chat");
    if (!bb_->run_until([this] { return turn_done_; }, effective_timeout_())) abandon_turn_();
```

with

```cpp
    {
      std::lock_guard<std::mutex> lk(turn_mu_());   // one turn at a time across ALL front-ends
      my_turn_ = true;
      turn_done_ = false;
      bb_->post("USER_MESSAGE", line, "chat");
      if (!bb_->run_until([this] { return turn_done_; }, effective_timeout_())) abandon_turn_();
      my_turn_ = false;
    }
```

and wrap both `/new` blocks' post+pump the same way (they mutate Arbiter state):

```cpp
    if (line == "/new") {
      std::lock_guard<std::mutex> lk(turn_mu_());
      bb_->post("NEW_SESSION", nlohmann::json::object(), "chat");
      bb_->pump();
      print_assistant_("[new session]");
      continue;
    }
```

- [ ] **Step 3: Build + FULL suite.** All existing Chat/e2e tests must stay green — `my_turn_` is true during every REPL-driven turn, so behavior is unchanged for the single-front-end paths the suite exercises. (The foreign-turn stdin hazard has no deterministic test seam without a blocking-stream harness — the guard is one line and review-verified; the symmetric Telegram-side guard IS tested in Task 5.)
- [ ] **Step 4: Commit.**

```bash
git add include/hades/module/chat_module.h src/module/chat_module.cpp
git commit -m "feat: ChatModule adopts the TurnGate + my_turn_ confirm guard (foreign turns never read stdin)"
```

---

## Task 3: Telegram parse/builder library (pure)

**Files:**
- Create: `include/hades/telegram/parse.h`, `src/telegram/parse.cpp`
- Test: `tests/test_telegram_parse.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**

```cpp
struct TgUpdate {
  long long   update_id = 0;
  std::string kind;            // "message" | "callback"
  long long   from_id = 0;     // sender user id (allowlist check)
  long long   chat_id = 0;     // where to reply
  std::string text;            // message text (kind=="message")
  std::string callback_id;     // callback_query.id (kind=="callback")
  std::string callback_data;   // "approve:<id>" | "deny:<id>" (kind=="callback")
};
struct ParsedUpdates { std::vector<TgUpdate> updates; bool ok = false; };
ParsedUpdates parse_updates(const std::string& body);                      // tolerant, never throws
std::vector<std::string> split_message(const std::string& text, std::size_t limit = 4096);
nlohmann::json build_send_message(long long chat_id, const std::string& text);
nlohmann::json build_confirm_message(long long chat_id, const std::string& prompt,
                                     const std::string& confirm_id);       // inline keyboard
nlohmann::json build_answer_callback(const std::string& callback_query_id);
```

- [ ] **Step 1: Write the failing tests** `tests/test_telegram_parse.cpp`:

```cpp
// tests/test_telegram_parse.cpp — pure Telegram parse/builder helpers
#include <gtest/gtest.h>
#include <string>
#include "hades/telegram/parse.h"
using namespace hades;

TEST(TelegramParse, ParsesTextMessage) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":7,"message":{"from":{"id":42},"chat":{"id":-100},"text":"hello"}}]})";
  auto p = parse_updates(body);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].kind, "message");
  EXPECT_EQ(p.updates[0].update_id, 7);
  EXPECT_EQ(p.updates[0].from_id, 42);
  EXPECT_EQ(p.updates[0].chat_id, -100);
  EXPECT_EQ(p.updates[0].text, "hello");
}

TEST(TelegramParse, ParsesCallbackQuery) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":8,"callback_query":{"id":"cbq1","from":{"id":42},"data":"approve:c1",
     "message":{"chat":{"id":-100}}}}]})";
  auto p = parse_updates(body);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].kind, "callback");
  EXPECT_EQ(p.updates[0].callback_id, "cbq1");
  EXPECT_EQ(p.updates[0].callback_data, "approve:c1");
  EXPECT_EQ(p.updates[0].from_id, 42);
  EXPECT_EQ(p.updates[0].chat_id, -100);
}

TEST(TelegramParse, SkipsNonTextAndMalformedEntries) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":9,"message":{"from":{"id":1},"chat":{"id":2}}},
    {"update_id":10,"message":{"from":"bad","chat":{"id":2},"text":"x"}},
    {"update_id":11},
    {"update_id":12,"message":{"from":{"id":5},"chat":{"id":6},"text":"good"}}]})";
  auto p = parse_updates(body);                       // photo/malformed entries skipped
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].text, "good");
}

TEST(TelegramParse, BadBodyIsNotOkAndNeverThrows) {
  EXPECT_FALSE(parse_updates("not json").ok);
  EXPECT_FALSE(parse_updates(R"({"ok":false})").ok);
  EXPECT_FALSE(parse_updates("").ok);
}

TEST(TelegramParse, SplitMessageRespectsLimit) {
  EXPECT_TRUE(split_message("").empty());
  EXPECT_EQ(split_message(std::string(4096, 'a')).size(), 1u);
  auto two = split_message(std::string(4097, 'a'));
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[0].size(), 4096u);
  EXPECT_EQ(two[1].size(), 1u);
  EXPECT_EQ(split_message("abcdef", 2), (std::vector<std::string>{"ab", "cd", "ef"}));
}

TEST(TelegramParse, BuildersProduceExactApiShapes) {
  auto msg = build_send_message(-100, "hi");
  EXPECT_EQ(msg["chat_id"], -100);
  EXPECT_EQ(msg["text"], "hi");
  auto conf = build_confirm_message(-100, "run shell?", "c1");
  EXPECT_EQ(conf["chat_id"], -100);
  EXPECT_EQ(conf["text"], "run shell?");
  const auto& row = conf["reply_markup"]["inline_keyboard"][0];
  EXPECT_EQ(row[0]["text"], "Approve");
  EXPECT_EQ(row[0]["callback_data"], "approve:c1");
  EXPECT_EQ(row[1]["text"], "Deny");
  EXPECT_EQ(row[1]["callback_data"], "deny:c1");
  EXPECT_EQ(build_answer_callback("cbq1")["callback_query_id"], "cbq1");
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add near the skills scan lines:

```cmake
target_sources(hades_core PRIVATE src/telegram/parse.cpp)
target_sources(hades_tests PRIVATE tests/test_telegram_parse.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/telegram/parse.h`:

```cpp
// include/hades/telegram/parse.h — pure Telegram Bot API parse/builder helpers
//
// parse_updates turns a getUpdates response body into typed TgUpdates (tolerant: malformed or
// non-text entries are skipped; a bad body -> ok=false; NEVER throws). split_message chunks a
// reply to Telegram's 4096-char message limit. The build_* helpers produce the exact JSON
// bodies for sendMessage / the inline-keyboard confirm / answerCallbackQuery, so the network
// layer (cpr) stays a thin, logic-free shell and tests pin the API shapes here.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {

struct TgUpdate {
  long long   update_id = 0;
  std::string kind;            // "message" | "callback"
  long long   from_id = 0;     // sender user id (allowlist check)
  long long   chat_id = 0;     // where to reply
  std::string text;            // message text (kind=="message")
  std::string callback_id;     // callback_query.id (kind=="callback")
  std::string callback_data;   // "approve:<id>" | "deny:<id>" (kind=="callback")
};

struct ParsedUpdates {
  std::vector<TgUpdate> updates;
  bool ok = false;             // false: body unparseable or Telegram replied ok!=true
};

ParsedUpdates parse_updates(const std::string& body);
std::vector<std::string> split_message(const std::string& text, std::size_t limit = 4096);
nlohmann::json build_send_message(long long chat_id, const std::string& text);
nlohmann::json build_confirm_message(long long chat_id, const std::string& prompt,
                                     const std::string& confirm_id);
nlohmann::json build_answer_callback(const std::string& callback_query_id);

}  // namespace hades
```

`src/telegram/parse.cpp`:

```cpp
// src/telegram/parse.cpp — tolerant getUpdates parse + reply chunking + request builders
#include "hades/telegram/parse.h"
namespace hades {
namespace {
// Type-safe numeric extraction: {"id":42} -> 42; missing/non-number -> 0 (entry then skipped).
long long num(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : 0;
}
}  // namespace

ParsedUpdates parse_updates(const std::string& body) {
  ParsedUpdates out;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.value("ok", false)) return out;
  auto res = j.find("result");
  if (res == j.end() || !res->is_array()) return out;
  out.ok = true;
  for (const auto& u : *res) {
    if (!u.is_object()) continue;
    TgUpdate t;
    t.update_id = num(u, "update_id");
    if (t.update_id == 0) continue;
    if (auto m = u.find("message"); m != u.end() && m->is_object()) {
      auto txt = m->find("text");
      if (txt == m->end() || !txt->is_string()) continue;      // photos/stickers etc: skip
      if (!m->contains("from") || !(*m)["from"].is_object()) continue;
      if (!m->contains("chat") || !(*m)["chat"].is_object()) continue;
      t.kind = "message";
      t.from_id = num((*m)["from"], "id");
      t.chat_id = num((*m)["chat"], "id");
      t.text = txt->get<std::string>();
      if (t.from_id == 0 || t.chat_id == 0) continue;
      out.updates.push_back(std::move(t));
    } else if (auto c = u.find("callback_query"); c != u.end() && c->is_object()) {
      t.kind = "callback";
      if (auto id = c->find("id"); id != c->end() && id->is_string())
        t.callback_id = id->get<std::string>();
      if (auto d = c->find("data"); d != c->end() && d->is_string())
        t.callback_data = d->get<std::string>();
      if (c->contains("from") && (*c)["from"].is_object()) t.from_id = num((*c)["from"], "id");
      if (c->contains("message") && (*c)["message"].is_object() &&
          (*c)["message"].contains("chat") && (*c)["message"]["chat"].is_object())
        t.chat_id = num((*c)["message"]["chat"], "id");
      if (t.callback_id.empty() || t.from_id == 0) continue;
      out.updates.push_back(std::move(t));
    }
  }
  return out;
}

std::vector<std::string> split_message(const std::string& text, std::size_t limit) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < text.size(); i += limit) out.push_back(text.substr(i, limit));
  return out;
}

nlohmann::json build_send_message(long long chat_id, const std::string& text) {
  return {{"chat_id", chat_id}, {"text", text}};
}

nlohmann::json build_confirm_message(long long chat_id, const std::string& prompt,
                                     const std::string& confirm_id) {
  nlohmann::json row = nlohmann::json::array(
      {{{"text", "Approve"}, {"callback_data", "approve:" + confirm_id}},
       {{"text", "Deny"}, {"callback_data", "deny:" + confirm_id}}});
  return {{"chat_id", chat_id},
          {"text", prompt},
          {"reply_markup", {{"inline_keyboard", nlohmann::json::array({row})}}}};
}

nlohmann::json build_answer_callback(const std::string& callback_query_id) {
  return {{"callback_query_id", callback_query_id}};
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R TelegramParse` → 6/6; FULL suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/telegram/parse.h src/telegram/parse.cpp tests/test_telegram_parse.cpp CMakeLists.txt
git commit -m "feat: telegram parse/builder lib (tolerant getUpdates parse, 4096 split, inline-keyboard bodies)"
```

---

## Task 4: `TelegramApi` seam + cpr implementation

**Files:**
- Create: `include/hades/telegram/api.h`, `include/hades/telegram/cpr_telegram_api.h`, `src/telegram/cpr_telegram_api.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TgUpdate`/`parse_updates`/builders (T3).
- Produces:

```cpp
class TelegramApi {
 public:
  virtual ~TelegramApi() = default;
  virtual std::vector<TgUpdate> get_updates(long long offset, double timeout_s) = 0;
  virtual bool send_message(long long chat_id, const std::string& text) = 0;
  virtual bool send_confirm(long long chat_id, const std::string& prompt,
                            const std::string& confirm_id) = 0;
  virtual void answer_callback(const std::string& callback_query_id) = 0;
};
class CprTelegramApi : public TelegramApi { explicit CprTelegramApi(std::string token); ... };
```

No unit tests for the cpr shell (network glue — `cpr_http` precedent); all logic lives in T3's tested builders/parse.

- [ ] **Step 1: Implement.** `include/hades/telegram/api.h`:

```cpp
// include/hades/telegram/api.h — Telegram Bot API seam (real impl: cpr; tests: scripted fake)
//
// The TelegramModule talks ONLY to this interface, so its whole turn/confirm/allowlist logic is
// testable without a network (the HttpClient-in-provider precedent). get_updates returns already-
// parsed updates ({} on any error — fail-soft); send_* return false on failure (module logs and
// carries on; the turn's history is already persisted by the Arbiter regardless).
#pragma once
#include <string>
#include <vector>
#include "hades/telegram/parse.h"  // TgUpdate
namespace hades {
class TelegramApi {
 public:
  virtual ~TelegramApi() = default;
  virtual std::vector<TgUpdate> get_updates(long long offset, double timeout_s) = 0;
  virtual bool send_message(long long chat_id, const std::string& text) = 0;
  virtual bool send_confirm(long long chat_id, const std::string& prompt,
                            const std::string& confirm_id) = 0;
  virtual void answer_callback(const std::string& callback_query_id) = 0;
};
}  // namespace hades
```

`include/hades/telegram/cpr_telegram_api.h`:

```cpp
// include/hades/telegram/cpr_telegram_api.h — real Bot API transport over HTTPS (cpr)
//
// Thin, logic-free shell: URLs are https://api.telegram.org/bot<token>/<method>; bodies come
// from the tested build_* helpers; responses go through the tested parse_updates. The token is
// embedded in every URL — hades_main adds it to the Eventlog redaction, and errors logged here
// must never print the URL. Every failure path degrades (empty result / false), never throws.
#pragma once
#include <string>
#include <vector>
#include "hades/telegram/api.h"
namespace hades {
class CprTelegramApi : public TelegramApi {
 public:
  explicit CprTelegramApi(std::string token);
  std::vector<TgUpdate> get_updates(long long offset, double timeout_s) override;
  bool send_message(long long chat_id, const std::string& text) override;
  bool send_confirm(long long chat_id, const std::string& prompt,
                    const std::string& confirm_id) override;
  void answer_callback(const std::string& callback_query_id) override;

 private:
  bool post_json_(const std::string& method, const nlohmann::json& body, double timeout_s);
  std::string base_;   // https://api.telegram.org/bot<token>
};
}  // namespace hades
```

`src/telegram/cpr_telegram_api.cpp`:

```cpp
// src/telegram/cpr_telegram_api.cpp — cpr glue for the Telegram Bot API (fail-soft, no logic)
#include "hades/telegram/cpr_telegram_api.h"
#include <cpr/cpr.h>
#include <iostream>
namespace hades {
namespace {
constexpr double kSendTimeoutS = 30.0;   // sendMessage/answerCallbackQuery are quick calls
}

CprTelegramApi::CprTelegramApi(std::string token)
    : base_("https://api.telegram.org/bot" + std::move(token)) {}

std::vector<TgUpdate> CprTelegramApi::get_updates(long long offset, double timeout_s) {
  // Long-poll: Telegram holds the request up to timeout_s; the cpr cap sits above it so a
  // full-length poll is never cut off client-side. Errors -> {} (the caller backs off).
  nlohmann::json body{{"offset", offset}, {"timeout", static_cast<long long>(timeout_s)}};
  auto r = cpr::Post(cpr::Url{base_ + "/getUpdates"},
                     cpr::Header{{"Content-Type", "application/json"}},
                     cpr::Body{body.dump()},
                     cpr::Timeout{static_cast<int>((timeout_s + 10.0) * 1000)});
  if (r.status_code != 200) {
    std::cerr << "hades: telegram getUpdates failed (status " << r.status_code << ")\n";
    return {};
  }
  auto p = parse_updates(r.text);
  if (!p.ok) std::cerr << "hades: telegram getUpdates: unparseable response\n";
  return p.updates;
}

bool CprTelegramApi::post_json_(const std::string& method, const nlohmann::json& body,
                                double timeout_s) {
  auto r = cpr::Post(cpr::Url{base_ + "/" + method},
                     cpr::Header{{"Content-Type", "application/json"}},
                     cpr::Body{body.dump()},
                     cpr::Timeout{static_cast<int>(timeout_s * 1000)});
  if (r.status_code != 200) {
    // Log the METHOD only — the URL carries the bot token.
    std::cerr << "hades: telegram " << method << " failed (status " << r.status_code << ")\n";
    return false;
  }
  return true;
}

bool CprTelegramApi::send_message(long long chat_id, const std::string& text) {
  return post_json_("sendMessage", build_send_message(chat_id, text), kSendTimeoutS);
}

bool CprTelegramApi::send_confirm(long long chat_id, const std::string& prompt,
                                  const std::string& confirm_id) {
  return post_json_("sendMessage", build_confirm_message(chat_id, prompt, confirm_id),
                    kSendTimeoutS);
}

void CprTelegramApi::answer_callback(const std::string& callback_query_id) {
  post_json_("answerCallbackQuery", build_answer_callback(callback_query_id), kSendTimeoutS);
}
}  // namespace hades
```

CMake:

```cmake
target_sources(hades_core PRIVATE src/telegram/cpr_telegram_api.cpp)
```

- [ ] **Step 2: Build + FULL suite** (compile-only gate for this task — no new tests; logic was tested in T3).
- [ ] **Step 3: Commit.**

```bash
git add include/hades/telegram/api.h include/hades/telegram/cpr_telegram_api.h src/telegram/cpr_telegram_api.cpp CMakeLists.txt
git commit -m "feat: TelegramApi seam + cpr transport (fail-soft, token never logged)"
```

---

## Task 5: `TelegramModule`

**Files:**
- Create: `include/hades/module/telegram_module.h`, `src/module/telegram_module.cpp`
- Test: `tests/test_telegram_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TelegramApi` (T4), `TurnGate` (T1), `split_message` (T3), bus keys `USER_MESSAGE`/`ASSISTANT_MESSAGE`/`CONFIRM_REQUEST`/`CONFIRM_RESPONSE`/`TURN_ABANDONED`, `Blackboard::run_until`, `kDefaultTurnIdleTimeoutS`.
- Produces:

```cpp
class TelegramModule : public Module {
 public:
  TelegramModule() = default;
  explicit TelegramModule(std::unique_ptr<TelegramApi> api);   // test injection (skips token env)
  ~TelegramModule() override;                                  // stop + join the poll thread
  std::string type() const override { return "telegram"; }
  void on_start(const Block& cfg, Blackboard& bb) override;    // MalConfig: allow_users/token
  void on_attach(Blackboard& bb) override;                     // captures only; NO thread
  void set_turn_gate(TurnGate* g);
  void set_turn_timeout_s(double s);
  void start_polling();                                        // spawn the loop (hades_main)
  void wait();                                                 // join (telegram-only roster blocks)
  bool poll_once();                                            // one batch; test seam + loop body
};
```

- [ ] **Step 1: Write the failing tests** `tests/test_telegram_module.cpp`:

```cpp
// tests/test_telegram_module.cpp — TelegramModule turn/confirm/allowlist logic over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/telegram_module.h"
using namespace hades;

namespace {
struct FakeApi : TelegramApi {
  std::deque<std::vector<TgUpdate>> batches;       // popped per get_updates call
  std::vector<std::pair<long long, std::string>> sent;         // send_message calls
  std::vector<std::string> confirms;                            // confirm_ids sent
  std::vector<std::string> answered;                            // callback ids answered
  bool throw_next = false;
  std::vector<TgUpdate> get_updates(long long, double) override {
    if (throw_next) { throw_next = false; throw std::runtime_error("net down"); }
    if (batches.empty()) return {};
    auto b = batches.front();
    batches.pop_front();
    return b;
  }
  bool send_message(long long chat, const std::string& t) override {
    sent.push_back({chat, t});
    return true;
  }
  bool send_confirm(long long, const std::string&, const std::string& id) override {
    confirms.push_back(id);
    return true;
  }
  void answer_callback(const std::string& id) override { answered.push_back(id); }
};

TgUpdate msg(long long uid, long long from, long long chat, const std::string& text) {
  TgUpdate u; u.update_id = uid; u.kind = "message"; u.from_id = from; u.chat_id = chat; u.text = text;
  return u;
}
TgUpdate cb(long long uid, long long from, long long chat, const std::string& cbid,
            const std::string& data) {
  TgUpdate u; u.update_id = uid; u.kind = "callback"; u.from_id = from; u.chat_id = chat;
  u.callback_id = cbid; u.callback_data = data;
  return u;
}

// Build a module over the fake api. allow_users = "42". `echo=true` installs a plain
// echo agent; tests that script their OWN bus behavior (confirm flow, long reply) pass
// false so the echo can't satisfy got_reply_ before the path under test runs.
struct Rig {
  Blackboard bb;
  FakeApi* api;
  std::unique_ptr<TelegramModule> mod;
  explicit Rig(bool echo = true) {
    auto a = std::make_unique<FakeApi>();
    api = a.get();
    mod = std::make_unique<TelegramModule>(std::move(a));
    Block cfg; cfg.kv["allow_users"] = "42";
    mod->on_start(cfg, bb);
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
    // First poll_once drains-and-discards the startup backlog (empty here).
    mod->poll_once();
  }
};
}  // namespace

TEST(TelegramModule, AllowedMessageDrivesTurnAndReplies) {
  Rig r;
  r.api->batches.push_back({msg(1, 42, -9, "hi")});
  EXPECT_TRUE(r.mod->poll_once());
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, -9);
  EXPECT_EQ(r.api->sent[0].second, "echo:hi");
}

TEST(TelegramModule, NonAllowedSenderSilentlyIgnored) {
  Rig r;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->batches.push_back({msg(1, 666, -9, "open sesame")});
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);                 // never reached the agent
  EXPECT_TRUE(r.api->sent.empty());       // and got no reply (don't reveal the bot is alive)
}

TEST(TelegramModule, StartupBacklogIsDrainedAndDiscarded) {
  Blackboard bb;
  auto a = std::make_unique<FakeApi>();
  FakeApi* api = a.get();
  auto mod = std::make_unique<TelegramModule>(std::move(a));
  Block cfg; cfg.kv["allow_users"] = "42";
  mod->on_start(cfg, bb);
  mod->on_attach(bb);
  bool user_msg = false;
  bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  api->batches.push_back({msg(1, 42, -9, "stale command from yesterday")});
  api->batches.push_back({msg(2, 42, -9, "another stale one")});
  mod->poll_once();                        // drain pass: consumes until empty, DISCARDS all
  EXPECT_FALSE(user_msg);
  EXPECT_TRUE(api->sent.empty());
  api->batches.push_back({msg(3, 42, -9, "fresh")});
  mod->poll_once();                        // now live
  EXPECT_TRUE(user_msg);
}

TEST(TelegramModule, LongReplyIsSplitAt4096) {
  Rig r(false);                                    // own handler below, no echo
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", std::string(5000, 'x'), "t");
  });
  r.api->batches.push_back({msg(1, 42, -9, "big")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 2u);
  EXPECT_EQ(r.api->sent[0].second.size(), 4096u);
  EXPECT_EQ(r.api->sent[1].second.size(), 904u);
}

TEST(TelegramModule, ConfirmFlowApproveViaInlineKeyboard) {
  Rig r(false);                                    // scripted confirm path, no echo
  // Script: the user message gates on a confirm; approval yields the final answer.
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "run shell?"}}, "arbiter");
  });
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    if (e.value.value("approved", false))
      r.bb.post("ASSISTANT_MESSAGE", "ran it", "t");
  });
  r.api->batches.push_back({msg(1, 42, -9, "wipe build dir")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->confirms.size(), 1u);          // buttons sent
  EXPECT_EQ(r.api->confirms[0], "c1");
  EXPECT_TRUE(r.api->sent.empty());               // no reply yet — gated
  r.api->batches.push_back({cb(2, 42, -9, "cbq9", "approve:c1")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->answered.size(), 1u);          // spinner dismissed
  EXPECT_EQ(r.api->answered[0], "cbq9");
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].second, "ran it");
}

TEST(TelegramModule, StaleOrUnknownCallbackIsAnsweredButNotPosted) {
  Rig r;
  bool confirm_resp = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { confirm_resp = true; });
  r.api->batches.push_back({cb(1, 42, -9, "cbq1", "approve:ghost")});
  r.mod->poll_once();
  EXPECT_EQ(r.api->answered.size(), 1u);          // always dismiss the spinner
  EXPECT_FALSE(confirm_resp);                     // but no CONFIRM_RESPONSE for an unknown id
}

TEST(TelegramModule, ForeignTurnConfirmIsNotCaptured) {
  Rig r;
  // A confirm from a REPL/web-driven turn (module idle, my_turn_ false) must not send buttons.
  r.bb.post("CONFIRM_REQUEST", {{"id", "zz"}, {"prompt", "?"}}, "arbiter");
  r.bb.pump();
  EXPECT_TRUE(r.api->confirms.empty());
}

TEST(TelegramModule, ApiErrorSurvivesAndReportsFailure) {
  Rig r;
  r.api->throw_next = true;
  EXPECT_FALSE(r.mod->poll_once());               // error surfaced, no crash
  r.api->batches.push_back({msg(1, 42, -9, "still alive")});
  EXPECT_TRUE(r.mod->poll_once());                // next batch works
  EXPECT_EQ(r.api->sent.size(), 1u);
}

TEST(TelegramModule, MissingOrBadAllowUsersIsMalConfig) {
  Blackboard bb;
  {
    TelegramModule m(std::make_unique<FakeApi>());
    Block cfg;                                     // no allow_users at all
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
  {
    TelegramModule m(std::make_unique<FakeApi>());
    Block cfg; cfg.kv["allow_users"] = "12ab";     // non-numeric id
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add near the other module test lines:

```cmake
target_sources(hades_core PRIVATE src/module/telegram_module.cpp)
target_sources(hades_tests PRIVATE tests/test_telegram_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/telegram_module.h`:

```cpp
// include/hades/module/telegram_module.h — Telegram front-end app (comms-interface analogue)
//
// Long-polls the Bot API on its own thread and drives whole turns through the shared TurnGate,
// exactly like the REPL/HTTP front-ends: lock -> post USER_MESSAGE -> run_until(reply|confirm)
// -> sendMessage (split at 4096). Confirm-gated actions become an inline-keyboard message
// ([Approve]/[Deny] -> callback_query -> CONFIRM_RESPONSE). Security: allow_users (numeric ids)
// is REQUIRED (MalConfig without it) and non-allowed senders are silently dropped; the bot
// token comes from an env var (token_env) and never appears in the manifest. The startup
// backlog is drained AND DISCARDED so commands queued while the agent was down never replay.
// The poll thread is started EXPLICITLY (start_polling, from hades_main) — never by on_attach —
// and is stop+joined in the dtor (before the Blackboard dies; embedding-timer precedent).
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "hades/module.h"
#include "hades/telegram/api.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

class TelegramModule : public Module {
 public:
  TelegramModule() = default;
  explicit TelegramModule(std::unique_ptr<TelegramApi> api);   // test injection (skips token env)
  ~TelegramModule() override;                                  // stop + join the poll thread
  std::string type() const override { return "telegram"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  // Shared whole-turn serializer (null -> module-local fallback).
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  // run_until idle ceiling (0 -> default kDefaultTurnIdleTimeoutS); wiring sets the manifest value.
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  void start_polling();   // spawn the poll loop (called by hades_main when rostered)
  void wait();            // join the poll thread (telegram-only roster blocks here; Ctrl-C exits)
  // Process ONE get_updates batch synchronously (the loop body; public as the test seam).
  // First call drains-and-discards the startup backlog. Returns false on an api/parse error
  // (the loop backs off 5s before retrying).
  bool poll_once();

 private:
  void run_loop_();
  void drive_turn_(long long chat_id, const nlohmann::json& post_value, const char* key);
  void handle_text_(const TgUpdate& u);
  void handle_callback_(const TgUpdate& u);
  void send_reply_(long long chat_id, const std::string& text);
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::unique_ptr<TelegramApi> api_;
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  std::set<long long> allow_;            // REQUIRED numeric user ids
  double poll_timeout_s_ = 50.0;
  double turn_timeout_override_s_ = 0.0;
  long long offset_ = 0;                 // next update id to fetch (in-memory, v1)
  bool drained_ = false;                 // startup backlog discarded yet?

  // Turn-capture state (poll thread only, under the gate while a turn runs).
  bool my_turn_ = false;                 // this module is driving the current turn
  bool got_reply_ = false;
  std::string last_reply_;
  nlohmann::json pending_confirm_;       // confirm captured during MY turn (null otherwise)
  std::string outstanding_confirm_id_;   // confirm sent to Telegram, awaiting callback
  long long outstanding_chat_id_ = 0;

  std::thread poll_thread_;
  std::atomic<bool> stop_{false};
  std::condition_variable stop_cv_;
  std::mutex stop_mu_;
};
}  // namespace hades
```

`src/module/telegram_module.cpp`:

```cpp
// src/module/telegram_module.cpp — poll loop, allowlist, turn driving, inline-keyboard confirms
#include "hades/module/telegram_module.h"
#include <chrono>
#include <exception>
#include <iostream>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"                       // MalConfig
#include "hades/telegram/cpr_telegram_api.h"
#include "hades/telegram/parse.h"                 // split_message
#include "hades/timeouts.h"                       // kDefaultTurnIdleTimeoutS
#include <cstdlib>
#include <sstream>
namespace hades {

TelegramModule::TelegramModule(std::unique_ptr<TelegramApi> api) : api_(std::move(api)) {}

TelegramModule::~TelegramModule() {
  stop_.store(true);
  stop_cv_.notify_all();
  // NOTE: a live get_updates long-poll can hold the join up to ~poll_timeout_s+10 (cpr cannot
  // be cancelled). Acceptable v1: Ctrl-C terminates the process; a /quit exit waits one poll.
  if (poll_thread_.joinable()) poll_thread_.join();
}

double TelegramModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void TelegramModule::on_start(const Block& cfg, Blackboard&) {
  // allow_users is REQUIRED and strictly numeric — an open bot means anyone who finds the
  // username can drive the agent's tools. Fail fast and loud (pin_fact precedent).
  if (!cfg.kv.count("allow_users"))
    throw MalConfig("telegram module requires allow_users (numeric Telegram user ids)");
  std::istringstream is(cfg.kv.at("allow_users"));
  std::string tok;
  while (is >> tok) {
    try {
      std::size_t pos = 0;
      long long id = std::stoll(tok, &pos);
      if (pos != tok.size()) throw std::invalid_argument(tok);
      allow_.insert(id);
    } catch (const std::exception&) {
      throw MalConfig("telegram allow_users: not a numeric user id: " + tok);
    }
  }
  if (allow_.empty())
    throw MalConfig("telegram module requires a non-empty allow_users");
  if (cfg.kv.count("poll_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("poll_timeout_s"), t)) poll_timeout_s_ = t;
  }
  if (api_) return;                               // injected (tests)
  const std::string env =
      cfg.kv.count("token_env") ? cfg.kv.at("token_env") : "TELEGRAM_BOT_TOKEN";
  const char* token = std::getenv(env.c_str());
  if (!token) throw MalConfig("telegram bot token env var not set: " + env);
  api_ = std::make_unique<CprTelegramApi>(token);
}

void TelegramModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Capture ONLY for turns this module drives (my_turn_) — a REPL/web turn's reply or confirm
  // is not ours to send to Telegram (turn-owner guard; symmetric to ChatModule's stdin guard).
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    pending_confirm_ = e.value;
  });
}

void TelegramModule::send_reply_(long long chat_id, const std::string& text) {
  for (const auto& chunk : split_message(text))
    if (!api_->send_message(chat_id, chunk))
      std::cerr << "hades: telegram sendMessage failed (reply dropped; history is persisted)\n";
}

// Shared turn driver for both entry points: lock the gate, reset capture, post the triggering
// event, run the turn to reply-or-confirm, then deliver the outcome to the chat.
void TelegramModule::drive_turn_(long long chat_id, const nlohmann::json& post_value,
                                 const char* key) {
  std::lock_guard<std::mutex> lk(turn_mu_());
  my_turn_ = true;
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post(key, post_value, "telegram");
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, effective_timeout_());
  if (!done) {
    // Idle timeout: abandon the turn (Arbiter bumps its epoch on TURN_ABANDONED) and say so.
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "telegram");
    bb_->pump();
    my_turn_ = false;
    send_reply_(chat_id, "[timed out]");
    return;
  }
  if (got_reply_) {
    my_turn_ = false;
    send_reply_(chat_id, last_reply_);
    return;
  }
  // Confirm-gated: send the inline keyboard and remember what we are waiting for. The
  // Arbiter's pending slot survives between turns (same contract as POST /confirm).
  const std::string id = pending_confirm_.value("id", "");
  const std::string prompt = pending_confirm_.value("prompt", "");
  my_turn_ = false;
  outstanding_confirm_id_ = id;
  outstanding_chat_id_ = chat_id;
  if (!api_->send_confirm(chat_id, prompt.empty() ? "confirm?" : prompt, id))
    std::cerr << "hades: telegram send_confirm failed (confirm still pending in the agent)\n";
}

void TelegramModule::handle_text_(const TgUpdate& u) {
  drive_turn_(u.chat_id, nlohmann::json(u.text), "USER_MESSAGE");
}

void TelegramModule::handle_callback_(const TgUpdate& u) {
  api_->answer_callback(u.callback_id);           // always dismiss the client spinner
  const bool approve = u.callback_data.rfind("approve:", 0) == 0;
  const bool deny = u.callback_data.rfind("deny:", 0) == 0;
  if (!approve && !deny) return;
  const std::string id = u.callback_data.substr(u.callback_data.find(':') + 1);
  if (id.empty() || id != outstanding_confirm_id_) return;   // stale/unknown: dismissed only
  outstanding_confirm_id_.clear();
  drive_turn_(outstanding_chat_id_, nlohmann::json{{"id", id}, {"approved", approve}},
              "CONFIRM_RESPONSE");
}

bool TelegramModule::poll_once() {
  try {
    if (!drained_) {
      // Startup backlog: consume until empty and DISCARD — commands queued while the agent
      // was down (Telegram keeps updates 24h) must not replay against the live agent.
      for (;;) {
        auto stale = api_->get_updates(offset_, 0.0);
        if (stale.empty()) break;
        for (const auto& u : stale) offset_ = std::max(offset_, u.update_id + 1);
      }
      drained_ = true;
      return true;
    }
    auto updates = api_->get_updates(offset_, poll_timeout_s_);
    for (const auto& u : updates) {
      offset_ = std::max(offset_, u.update_id + 1);
      if (!allow_.count(u.from_id)) continue;      // silently drop non-allowed senders
      if (u.kind == "message" && !u.text.empty()) handle_text_(u);
      else if (u.kind == "callback") handle_callback_(u);
    }
    return true;
  } catch (const std::exception& e) {
    std::cerr << "hades: telegram poll error: " << e.what() << "\n";
    return false;
  } catch (...) {
    std::cerr << "hades: telegram poll error (unknown)\n";
    return false;
  }
}

void TelegramModule::run_loop_() {
  while (!stop_.load()) {
    const bool ok = poll_once();
    if (!ok && !stop_.load()) {
      // Backoff on error; interruptible so the dtor never waits the full 5s.
      std::unique_lock<std::mutex> lk(stop_mu_);
      stop_cv_.wait_for(lk, std::chrono::seconds(5), [this] { return stop_.load(); });
    }
  }
}

void TelegramModule::start_polling() {
  if (poll_thread_.joinable()) return;             // idempotent
  poll_thread_ = std::thread([this] { run_loop_(); });
}

void TelegramModule::wait() {
  if (poll_thread_.joinable()) poll_thread_.join();
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R TelegramModule` → 9/9; FULL suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/telegram_module.h src/module/telegram_module.cpp tests/test_telegram_module.cpp CMakeLists.txt
git commit -m "feat: TelegramModule — long-poll front-end (allowlist, backlog discard, gate turns, inline-keyboard confirms)"
```

---

## Task 6: Wiring + hades_main + ship

**Files:**
- Modify: `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`, `manifests/dev.hades`, `.gitignore`, `CLAUDE.md`
- Test: `tests/test_telegram_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TelegramModule` (T5), `TurnGate` (T1).
- Produces: `Agent::gate` (**FIRST** member) + `Agent::telegram` (after `serve`, before `executor` — executor stays LAST); roster factory `"telegram"`; `Telegram` block; gate injected into chat/serve/telegram before their `on_attach`.

- [ ] **Step 1: Write the failing tests** `tests/test_telegram_wiring.cpp`:

```cpp
// tests/test_telegram_wiring.cpp — manifest wiring for the telegram front-end
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

namespace {
std::string manifest_text(const std::string& telegram_block) {
  return std::string("Session\n{\n  model = m\n}\n") + "Module = arbiter\n" +
         "Module = telegram\n" + telegram_block;
}
}  // namespace

TEST(TelegramWiring, RosterBuildsModuleWithTokenAndAllowlist) {
  ::setenv("HADES_TEST_TG_TOKEN", "tok", 1);
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_TOKEN\n  allow_users = 42\n}\n"));
  Agent agent = build_agent(bb, m);
  EXPECT_NE(agent.telegram, nullptr);
}

TEST(TelegramWiring, MissingAllowUsersIsMalConfig) {
  ::setenv("HADES_TEST_TG_TOKEN", "tok", 1);
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_TOKEN\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TelegramWiring, MissingTokenEnvIsMalConfig) {
  ::unsetenv("HADES_TEST_TG_MISSING");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_MISSING\n  allow_users = 42\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TelegramWiring, NoTelegramRosterLeavesMemberNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.telegram, nullptr);
}
```

- [ ] **Step 2: CMake + run — expect FAIL** (`Agent` has no member `telegram`). Add:

```cmake
target_sources(hades_tests PRIVATE tests/test_telegram_wiring.cpp)
```

- [ ] **Step 3: Implement wiring.** `app/agent_wiring.h`: add includes `"hades/turn_gate.h"` and `"hades/module/telegram_module.h"`. In `struct Agent`:
  - add as the **FIRST** member (before `llm`), with comment:

```cpp
  // Shared whole-turn serializer. FIRST member => destroyed LAST — it must outlive every
  // front-end module that holds a pointer to it (members destruct in reverse order).
  TurnGate gate;
```

  - add after `serve` (executor stays LAST):

```cpp
  std::unique_ptr<TelegramModule> telegram;   // optional Telegram front-end (long-poll thread)
```

`app/agent_wiring.cpp`, inside `wire_agent` (signature gains a trailing `const Block& telegram_cfg = Block{}` after `skills_cfg`):
  - in step 4 (chat) and step 5 (serve), inject the gate BEFORE `on_attach`:

```cpp
  if (a.chat) {
    a.chat->set_turn_gate(&a.gate);
    a.chat->on_attach(bb);
  }
```

```cpp
  if (a.serve) {
    a.serve->set_turn_gate(&a.gate);
    a.serve->on_attach(bb);
  }
```

  - add step 6 after serve:

```cpp
  // 6) Telegram front-end: config (MalConfig on missing allow_users / token env) + captures.
  //    The poll thread is NOT started here — hades_main calls start_polling() explicitly, so
  //    tests and non-interactive builds never spawn a surprise thread.
  if (a.telegram) {
    a.telegram->set_turn_gate(&a.gate);
    a.telegram->on_start(telegram_cfg, bb);
    a.telegram->on_attach(bb);
  }
```

In the Manifest overload: `launcher.register_factory("telegram", []{ return std::make_unique<TelegramModule>(); });`, `a.telegram = take_as<TelegramModule>(launcher, "telegram");`, extract the block:

```cpp
  const auto tg_blocks = m.of("Telegram");
  const Block telegram_cfg = tg_blocks.empty() ? Block{} : tg_blocks.front();
```

pass it as the new last `wire_agent` argument, and apply the idle ceiling with chat/serve:

```cpp
  if (a.telegram) a.telegram->set_turn_timeout_s(turn_idle_timeout_s);
```

The TEST overload stays unchanged (`a.telegram` never constructed → null; trailing default).

- [ ] **Step 4: hades_main.** In `app/hades_main.cpp`:
  - after `eventlog.add_redaction(key);`, redact the bot token when configured (best-effort — the module itself fail-fasts on a missing token):

```cpp
    // Redact the Telegram bot token too (it is embedded in every Bot API URL). Best-effort:
    // resolve the same env var the module will use; if unset, the module throws MalConfig later.
    {
      const auto tg = manifest.of("Telegram");
      std::string tg_env = "TELEGRAM_BOT_TOKEN";
      if (!tg.empty() && tg.front().kv.count("token_env")) tg_env = tg.front().kv.at("token_env");
      if (const char* tg_token = std::getenv(tg_env.c_str())) eventlog.add_redaction(tg_token);
    }
```

  - after the `if (resume) ...load_history();` line, start polling and handle the telegram-only roster:

```cpp
    // Telegram front-end: start the poll loop AFTER the full graph is wired (never inside
    // wire_agent — no surprise threads in tests). Runs alongside whichever blocking front-end
    // (REPL / --serve) drives the main thread; turns are serialized by the shared TurnGate.
    if (agent.telegram) agent.telegram->start_polling();
```

  - replace the front-end selection tail so a telegram-only roster blocks on the poll thread:

```cpp
    if (serve) {
      if (!agent.serve) { std::cerr << "hades: no `serve` module in the manifest Module roster\n"; return 1; }
      const ServeConfig cfg = resolve_serve_config(manifest, cli_port);
      agent.serve->listen(cfg.host, cfg.port, cfg.webroot);  // blocks until killed
    } else if (agent.chat) {
      agent.chat->run_repl(std::cin, std::cout);
    } else if (agent.telegram) {
      std::cerr << "hades: telegram-only roster — polling (Ctrl-C to exit)\n";
      agent.telegram->wait();                                 // blocks on the poll thread
    } else {
      std::cerr << "hades: no `chat` module in the manifest Module roster\n";
      return 1;
    }
```

- [ ] **Step 5: Ship files.**
  - `manifests/dev.hades` — append (commented; user uncomments + sets their id):

```
# --- Telegram front-end (uncomment, set YOUR numeric user id, export TELEGRAM_BOT_TOKEN) ---
# Get an id: message @userinfobot. Non-listed senders are silently ignored. Token via env ONLY
# (keep it in a gitignored .env you `source`); it is redacted in session.log.
# Module = telegram
# Telegram
# {
#   token_env      = TELEGRAM_BOT_TOKEN
#   allow_users    = 123456789
#   poll_timeout_s = 50
# }
```

  - `.gitignore` — add a line: `.env`
  - `CLAUDE.md` — current-state line (telegram front-end + new test count), a `### Telegram front-end` subsection (TurnGate concurrency model + turn-owner guard + allowlist fail-fast + backlog discard + inline-keyboard confirms + token redaction + .env pattern), Gotchas (allow_users REQUIRED; dtor join can wait one poll cycle; drain-discard on start), NEXT item 2 marked DONE.
- [ ] **Step 6: Full build + FULL suite** → all green (`-R Telegram` + existing serve/chat/e2e regressions).
- [ ] **Step 7: Commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp tests/test_telegram_wiring.cpp CMakeLists.txt manifests/dev.hades .gitignore CLAUDE.md
git commit -m "feat: wire telegram — Agent.gate + Agent.telegram, factory, token redaction, telegram-only mode"
```

---

## Verification (end-to-end)

1. FULL suite in `nix develop`: 284 baseline + ~20 new, all green (ASan/UBSan config as-is).
2. Manual live smoke (Vaios; needs a bot from @BotFather + your user id from @userinfobot):
   ```bash
   # .env (gitignored): HADES_API_KEY=... TELEGRAM_BOT_TOKEN=...
   set -a; source .env; set +a
   # uncomment the Telegram block in dev.hades, set allow_users to YOUR id
   nix develop --command ./build/hades manifests/dev.hades --serve
   # phone: message the bot -> reply arrives; browser at :8080 still works (shared gate)
   # phone: ask it to run a shell command -> [Approve][Deny] buttons -> approve -> result
   # message from another account -> silence
   # grep the token in session.log -> ***REDACTED***
   ```
3. Restart while messages queued → backlog discarded (no stale command executes).

## Execution

Subagent-driven development (house process): fresh implementer per task (opus per `feedback_sdd_implementer_opus`), per-task review, final whole-branch review, then finishing-a-development-branch (ff-merge to main; no remote, never push).
