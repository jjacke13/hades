# Auto-extract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A background module that reviews each human turn with a cheap aux LLM call and saves durable facts to archival memory automatically.

**Architecture:** `AutoExtractModule` subscribes the turn traffic, offloads one provider call per human turn to the Executor (LLMModule capture discipline: worker touches no module state except an atomic busy flag), appends parsed facts to the archival store with `"src":"auto"`, and posts `AUX_SPENT_USD` which LLMModule folds into the cumulative budget. Pure helpers (reply parse, digest build, artifact detect) live in their own header for direct testing.

**Tech Stack:** C++20, CMake+Ninja inside `nix develop`, nlohmann_json, GoogleTest.

Spec: `docs/superpowers/specs/2026-07-16-auto-extract-design.md` (committed on this branch).

## Global Constraints

- Every build/test command runs inside `nix develop`; baseline **672/672** green before Task 1. TSan lane (`build-tsan/`) must also be green at branch end (new thread surface).
- Branch `feat/auto-extract` (created; spec committed `0a0983c`). Commit style `<type>: <desc>`, NO attribution footer.
- Pump-thread handlers never throw; the worker path is fully try/catch-wrapped and must always clear the busy flag.
- The worker may capture ONLY non-owning pointers/values whose lifetime the Executor teardown order guarantees (LLMModule precedent): the provider raw pointer, the Blackboard pointer, plain values, and `std::atomic<bool>* busy` — never `this` for anything else.
- Human-origin turns only; bracketed turn artifacts (`[blocked`, `[declined`, `[stopped`, `[timed out`, `[new session]` prefixes) never reviewed.
- Facts: trimmed, newlines→spaces, 500-char cap, empties dropped, `max_facts` (default 3) clamp, exact-dup skip against the store; record `{"text","ts","src":"auto"}`.
- Digest: `"U: <user>\nA: <assistant>"`, each side truncated to 2000 bytes.
- Reply contract: `NONE` (case-insensitive, trimmed) or a JSON array of strings (tolerate a ```json fenced block); anything else → no facts.
- Do NOT touch `manifests/dev.local.hades`, `manifests/pi.hades`, `manifests/dev2.hades`, `memory/`, `skills/greek-greeting`, `skills/ponytail`, `build*/`.

## File Structure

```
include/hades/extract/extract.h          T1  pure helpers (parse/digest/artifact)
src/apps/auto_extract/extract.cpp        T1  implementations
tests/test_extract.cpp                   T1
src/apps/llm/llm.cpp                     T2  AUX_SPENT_USD fold (modify)
tests/test_llm_module.cpp                T2  (append)
include/hades/module/auto_extract_module.h  T3
src/apps/auto_extract/auto_extract.cpp   T3  the module
tests/test_auto_extract_module.cpp       T3
app/agent_wiring.h / .cpp                T4  member, factory, merged cfg, set_executor (modify)
tests/test_auto_extract_wiring.cpp       T4
docs/manifest-reference.md, manifests/dev.hades, CLAUDE.md  T4  ship
CMakeLists.txt                           T1, T3, T4
```

---

## Task 1: Pure extraction helpers

**Files:**
- Create: `include/hades/extract/extract.h`, `src/apps/auto_extract/extract.cpp`
- Test: `tests/test_extract.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**
- `std::vector<std::string> parse_extract_reply(const std::string& reply, std::size_t max_facts)`
- `std::string build_extract_digest(const std::string& user, const std::string& assistant)` — truncation at 2000 bytes per side
- `bool is_turn_artifact(const std::string& assistant_text)`

- [ ] **Step 1: Write the failing tests** `tests/test_extract.cpp`:

```cpp
// tests/test_extract.cpp — pure auto-extract helpers: reply parse, digest build, artifact gate
#include <gtest/gtest.h>
#include <string>
#include "hades/extract/extract.h"
using namespace hades;

TEST(Extract, ParsesJsonArrayOfStrings) {
  auto v = parse_extract_reply(R"(["user prefers metric", "timezone is EET"])", 3);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], "user prefers metric");
  EXPECT_EQ(v[1], "timezone is EET");
}

TEST(Extract, NoneAndGarbageYieldEmpty) {
  EXPECT_TRUE(parse_extract_reply("NONE", 3).empty());
  EXPECT_TRUE(parse_extract_reply("  none \n", 3).empty());
  EXPECT_TRUE(parse_extract_reply("", 3).empty());
  EXPECT_TRUE(parse_extract_reply("I think the user likes metric.", 3).empty());  // prose, not JSON
  EXPECT_TRUE(parse_extract_reply(R"({"facts":["x"]})", 3).empty());              // object, not array
  EXPECT_TRUE(parse_extract_reply("[1, 2, 3]", 3).empty());                       // non-string items dropped
}

TEST(Extract, ToleratesFencedBlockAndMixedItems) {
  auto v = parse_extract_reply("```json\n[\"kept\", 42, \"also kept\"]\n```", 5);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], "kept");
  EXPECT_EQ(v[1], "also kept");
}

TEST(Extract, ClampsCapsAndCleans) {
  auto v = parse_extract_reply(R"(["a", "b", "c", "d"])", 2);
  EXPECT_EQ(v.size(), 2u);                                        // max_facts clamp
  auto w = parse_extract_reply("[\"  line1\\nline2  \", \"\", \"   \"]", 5);
  ASSERT_EQ(w.size(), 1u);                                        // empties dropped
  EXPECT_EQ(w[0], "line1 line2");                                 // trimmed, newline -> space
  std::string big(1000, 'x');
  auto u = parse_extract_reply("[\"" + big + "\"]", 5);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].size(), 500u);                                   // 500-char cap
}

TEST(Extract, DigestBuildsAndTruncates) {
  EXPECT_EQ(build_extract_digest("hi", "hello"), "U: hi\nA: hello");
  const std::string d = build_extract_digest(std::string(3000, 'u'), std::string(3000, 'a'));
  EXPECT_EQ(d.size(), std::string("U: \nA: ").size() + 2000 + 2000);
}

TEST(Extract, ArtifactGate) {
  for (const char* a : {"[blocked: rm -rf]", "[declined by user]",
                        "[stopped: reached max tool steps]", "[timed out]", "[new session]"})
    EXPECT_TRUE(is_turn_artifact(a)) << a;
  EXPECT_FALSE(is_turn_artifact("[1] citation style answer"));
  EXPECT_FALSE(is_turn_artifact("normal answer"));
  EXPECT_FALSE(is_turn_artifact(""));
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add next to the other core sources / test lines:

```cmake
target_sources(hades_core PRIVATE src/apps/auto_extract/extract.cpp)
target_sources(hades_tests PRIVATE tests/test_extract.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/extract/extract.h`:

```cpp
// include/hades/extract/extract.h — pure helpers behind the auto-extract module
//
// parse_extract_reply: the aux model's reply contract is "NONE" or a JSON array of short
// strings (a ```json fence is tolerated); everything else parses to no facts (fail-closed —
// a confused model must never write garbage memories). build_extract_digest: the one-exchange
// review input. is_turn_artifact: bracketed Arbiter/front-end outcomes are not conversation.
#pragma once
#include <string>
#include <vector>
namespace hades {
std::vector<std::string> parse_extract_reply(const std::string& reply, std::size_t max_facts);
std::string build_extract_digest(const std::string& user, const std::string& assistant);
bool is_turn_artifact(const std::string& assistant_text);
}  // namespace hades
```

`src/apps/auto_extract/extract.cpp`:

```cpp
// src/apps/auto_extract/extract.cpp — pure auto-extract helpers (see the header)
#include "hades/extract/extract.h"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
namespace hades {
namespace {
constexpr std::size_t kDigestSideCap = 2000;   // bytes per digest side
constexpr std::size_t kFactCap       = 500;    // bytes per saved fact

std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

std::vector<std::string> parse_extract_reply(const std::string& reply, std::size_t max_facts) {
  std::vector<std::string> out;
  std::string r = trim(reply);
  // Strip a ``` / ```json fence if the whole reply is one fenced block.
  if (r.rfind("```", 0) == 0) {
    const std::size_t nl = r.find('\n');
    const std::size_t close = r.rfind("```");
    if (nl != std::string::npos && close != std::string::npos && close > nl)
      r = trim(r.substr(nl + 1, close - nl - 1));
  }
  if (r.empty()) return out;
  {  // case-insensitive NONE
    std::string low = r;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low == "none") return out;
  }
  const auto j = nlohmann::json::parse(r, nullptr, false);
  if (!j.is_array()) return out;                 // fail-closed: array or nothing
  for (const auto& item : j) {
    if (out.size() >= max_facts) break;
    if (!item.is_string()) continue;
    std::string t = item.get<std::string>();
    for (char& c : t)
      if (c == '\n' || c == '\r') c = ' ';       // one fact = one store line
    t = trim(t);
    if (t.empty()) continue;
    if (t.size() > kFactCap) t.resize(kFactCap);
    out.push_back(std::move(t));
  }
  return out;
}

std::string build_extract_digest(const std::string& user, const std::string& assistant) {
  return "U: " + user.substr(0, kDigestSideCap) + "\nA: " + assistant.substr(0, kDigestSideCap);
}

bool is_turn_artifact(const std::string& a) {
  for (const char* p : {"[blocked", "[declined", "[stopped", "[timed out", "[new session]"})
    if (a.rfind(p, 0) == 0) return true;
  return false;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R Extract` → pass; full suite green (672 + 6).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/extract/extract.h src/apps/auto_extract/extract.cpp tests/test_extract.cpp CMakeLists.txt
git commit -m "feat: auto-extract pure helpers — reply parse (fail-closed), digest build, artifact gate"
```

---

## Task 2: `AUX_SPENT_USD` budget fold in LLMModule

**Files:**
- Modify: `src/apps/llm/llm.cpp` (inside `LLMModule::on_attach`, after the existing `LLM_RESPONSE` budget handler)
- Test: `tests/test_llm_module.cpp` (append)

**Interfaces:**
- Consumes: bus key `AUX_SPENT_USD` (number — a per-call **delta**, posted by any aux consumer).
- Produces: the delta folded into the module's cumulative `spent_`, re-posted as `BUDGET_SPENT_USD` (unchanged single-writer semantics: this handler runs only on the pump thread).

- [ ] **Step 1: Write the failing test** — append to `tests/test_llm_module.cpp` (match its existing construction style — scripted provider injection; look at the first test in the file for the harness idiom):

```cpp
TEST(LLMModule, AuxSpendFoldsIntoCumulativeBudget) {
  // AUX_SPENT_USD is a per-call DELTA from an aux consumer (auto-extract); the LLMModule
  // folds it into the same cumulative spent_ the LLM_RESPONSE handler owns, so
  // stay_on_budget gates aux spend too. Non-numeric payloads are ignored.
  Blackboard bb;
  LLMModule m(std::make_unique<ScriptedProvider>());   // reuse the file's scripted provider
  Block cfg;
  cfg.kv["price_per_mtok"] = "2.0";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  double budget = -1.0;
  bb.subscribe("BUDGET_SPENT_USD", [&](const Entry& e) { budget = e.value.get<double>(); });
  bb.post("AUX_SPENT_USD", 0.25, "auto_extract");
  bb.post("AUX_SPENT_USD", "garbage", "x");            // ignored, no crash
  bb.post("AUX_SPENT_USD", 0.25, "auto_extract");
  bb.pump();
  EXPECT_DOUBLE_EQ(budget, 0.5);                       // two deltas accumulated
}
```

(If the file's scripted provider type has a different name/constructor, use that one — the assertion logic is what matters. If constructing the module without a provider is simpler in that file's idiom, `LLMModule m(nullptr)` + a cfg with `price_per_mtok` only is fine as long as `on_start` doesn't throw — check how existing tests avoid the api-key throw: they inject a provider.)

- [ ] **Step 2: Run — expect FAIL** (budget stays -1: nothing subscribes AUX_SPENT_USD).
- [ ] **Step 3: Implement.** In `src/apps/llm/llm.cpp`, directly after the existing `bb.subscribe("LLM_RESPONSE", …)` budget handler in `on_attach`, add:

```cpp
  // Aux-call spend (auto-extract's background reviews): posted as a per-call DELTA by the
  // aux consumer's worker. Folded here — the pump thread stays the single writer of spent_,
  // and BUDGET_SPENT_USD stays the one cumulative number stay_on_budget reads. This closes
  // the "background LLM calls are unmetered" class the embedding path still has (documented).
  bb.subscribe("AUX_SPENT_USD", [this](const Entry& e) {
    if (!e.value.is_number()) return;
    spent_ += e.value.get<double>();
    bb_->post("BUDGET_SPENT_USD", spent_, "llm");
  });
```

- [ ] **Step 4: Build + test.** `-R LLMModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add src/apps/llm/llm.cpp tests/test_llm_module.cpp
git commit -m "feat: LLMModule folds AUX_SPENT_USD deltas into the cumulative budget"
```

---

## Task 3: `AutoExtractModule`

**Files:**
- Create: `include/hades/module/auto_extract_module.h`, `src/apps/auto_extract/auto_extract.cpp`
- Test: `tests/test_auto_extract_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: T1 helpers; `Provider`/`LlmRequest`/`LlmResponse` (`hades/llm/provider.h`); `Executor` (`hades/executor.h`); bus keys `TURN_ORIGIN`, `USER_MESSAGE`, `ASSISTANT_MESSAGE`.
- Produces: `class AutoExtractModule : public Module` — `type()=="auto_extract"`; ctor `explicit AutoExtractModule(std::unique_ptr<Provider> p = nullptr)` (injected for tests; else built in `on_start`); `set_executor(Executor*)`; posts `AUX_SPENT_USD`; appends `{"text","ts","src":"auto"}` lines to the store.
- `on_start` cfg keys (the wiring passes a MERGED block, Task 4): `endpoint`, `api_key_env` (default `HADES_API_KEY`), `model`, `price_per_mtok`, `timeout_s` (default 60), `max_facts` (default 3), `store` (default `.hades/memory.jsonl`).

- [ ] **Step 1: Write the failing tests** `tests/test_auto_extract_module.cpp`:

```cpp
// tests/test_auto_extract_module.cpp — AutoExtractModule: human-turn review -> archival facts
//
// A scripted provider stands in for the aux LLM (no socket); no Executor is set, so the
// review runs INLINE during pump — every assertion is deterministic.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/module/auto_extract_module.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
struct ScriptedAux : Provider {
  std::string reply = R"(["user prefers metric"])";
  int calls = 0;
  std::string last_digest;
  LlmResponse complete(const LlmRequest& req) override {
    ++calls;
    if (!req.messages.empty())
      last_digest = req.messages.back().value("content", "");
    LlmResponse r;
    r.text = reply;
    r.prompt_tokens = 100000;      // 0.1 Mtok -> with price 2.0: 0.5 USD total w/ completion
    r.completion_tokens = 150000;
    return r;
  }
};

std::string fresh_store(const char* tag) {
  const std::string d =
      ::testing::TempDir() + "/autoex_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(d);
  fs::create_directories(d);
  return d + "/memory.jsonl";
}
std::vector<nlohmann::json> store_lines(const std::string& p) {
  std::vector<nlohmann::json> out;
  std::ifstream f(p);
  std::string l;
  while (std::getline(f, l)) {
    auto j = nlohmann::json::parse(l, nullptr, false);
    if (!j.is_discarded()) out.push_back(j);
  }
  return out;
}
// Drives one full turn's bus traffic.
void turn(Blackboard& bb, const std::string& origin, const std::string& user,
          const std::string& assistant) {
  bb.post("TURN_ORIGIN", origin, "test");
  bb.post("USER_MESSAGE", user, "test");
  bb.post("ASSISTANT_MESSAGE", assistant, "test");
  bb.pump();
}
AutoExtractModule* attach(Blackboard& bb, std::unique_ptr<ScriptedAux> prov,
                          const std::string& store,
                          std::vector<std::unique_ptr<AutoExtractModule>>& keep) {
  auto m = std::make_unique<AutoExtractModule>(std::move(prov));
  Block cfg;
  cfg.kv["store"] = store;
  cfg.kv["price_per_mtok"] = "2.0";
  m->on_start(cfg, bb);
  m->on_attach(bb);
  keep.push_back(std::move(m));
  return keep.back().get();
}
}  // namespace

TEST(AutoExtract, HumanTurnWritesFactWithProvenanceAndPostsAuxSpend) {
  Blackboard bb;
  const std::string store = fresh_store("basic");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  double aux = 0.0;
  bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value.get<double>(); });
  turn(bb, "human", "please use metric units", "noted, metric from now on");
  bb.pump();                                            // deliver the worker-posted AUX event
  EXPECT_EQ(p->calls, 1);
  EXPECT_NE(p->last_digest.find("U: please use metric units"), std::string::npos);
  auto lines = store_lines(store);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].value("text", ""), "user prefers metric");
  EXPECT_EQ(lines[0].value("src", ""), "auto");
  EXPECT_GT(lines[0].value("ts", 0.0), 0.0);
  EXPECT_DOUBLE_EQ(aux, (100000.0 + 150000.0) / 1e6 * 2.0);  // 0.5 — tokens × price
}

TEST(AutoExtract, NoneMeansNoWriteAndDupsSkipped) {
  Blackboard bb;
  const std::string store = fresh_store("none");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  p->reply = "NONE";
  turn(bb, "human", "hi", "hello");
  EXPECT_TRUE(store_lines(store).empty());
  p->reply = R"(["fact one"])";
  turn(bb, "human", "a", "b");
  turn(bb, "human", "c", "d");                          // same fact again -> dup skipped
  EXPECT_EQ(store_lines(store).size(), 1u);
}

TEST(AutoExtract, NonHumanOriginsAndArtifactsIgnored) {
  Blackboard bb;
  const std::string store = fresh_store("origin");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  turn(bb, "peer:pi0", "peer question", "peer answer");
  turn(bb, "heartbeat:daily", "tick prompt", "tick reply");
  turn(bb, "human", "real question", "[timed out]");    // artifact
  turn(bb, "human", "", "answer");                      // empty user
  EXPECT_EQ(p->calls, 0);
  EXPECT_TRUE(store_lines(store).empty());
}

TEST(AutoExtract, MaxFactsClampAndMalformedBusSafety) {
  Blackboard bb;
  const std::string store = fresh_store("clamp");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);             // default max_facts = 3
  p->reply = R"(["f1","f2","f3","f4","f5"])";
  turn(bb, "human", "q", "a");
  EXPECT_EQ(store_lines(store).size(), 3u);
  bb.post("TURN_ORIGIN", 42, "x");                      // malformed payloads: no crash
  bb.post("USER_MESSAGE", nlohmann::json::object(), "x");
  bb.post("ASSISTANT_MESSAGE", 7, "x");
  bb.pump();
  SUCCEED();
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add:

```cmake
target_sources(hades_core PRIVATE src/apps/auto_extract/auto_extract.cpp)
target_sources(hades_tests PRIVATE tests/test_auto_extract_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/auto_extract_module.h`:

```cpp
// include/hades/module/auto_extract_module.h — post-turn background memory harvest
//
// Reviews each HUMAN turn's last exchange with an aux LLM call (own provider, built from the
// merged cfg block the wiring passes — Session values + AutoExtract overrides) and appends the
// parsed facts to the archival store with src:"auto". The call runs on the Executor when set
// (never blocks the pump thread); at most ONE review is in flight (a turn finishing mid-review
// is skipped, never queued). Peer/heartbeat turns are never harvested (memory-pollution /
// injection surface). Spend is posted as an AUX_SPENT_USD delta; the LLMModule folds it into
// the cumulative budget. Fail-soft everywhere: an extractor error can never touch a turn.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class Executor;

class AutoExtractModule : public Module {
public:
  explicit AutoExtractModule(std::unique_ptr<Provider> p = nullptr) : provider_(std::move(p)) {}
  std::string type() const override { return "auto_extract"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* e) { executor_ = e; }

private:
  std::unique_ptr<Provider> provider_;   // injected (tests) or built in on_start
  Executor* executor_ = nullptr;         // nullptr -> review runs inline (tests)
  Blackboard* bb_ = nullptr;
  std::string store_ = ".hades/memory.jsonl";
  std::string model_;
  double price_per_mtok_ = 0.0;
  std::size_t max_facts_ = 3;
  std::string origin_ = "human";         // latest TURN_ORIGIN (pump thread only)
  std::string last_user_;                // latest USER_MESSAGE (pump thread only)
  std::atomic<bool> busy_{false};        // one review in flight; worker clears it
};
}  // namespace hades
```

`src/apps/auto_extract/auto_extract.cpp`:

```cpp
// src/apps/auto_extract/auto_extract.cpp — AutoExtractModule (see the header)
#include "hades/module/auto_extract_module.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/extract/extract.h"
#include "hades/launcher.h"  // MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
namespace hades {
namespace {
constexpr const char* kSystemPrompt =
    "You review one exchange between a user and their AI assistant. Extract durable facts "
    "worth remembering across sessions: user preferences, corrections the user made, and "
    "standing facts about the user or their environment. Ignore small talk, one-off task "
    "detail, and anything already obvious. Reply with ONLY a JSON array of short "
    "self-contained strings, or NONE if nothing qualifies.";

// Append accepted facts (exact-dup-skipped against the store) and return the count written.
// Runs on the WORKER; touches only its arguments. Same line-append discipline as the
// save_memory tool; the tolerant loaders skip a hypothetically torn line (spec-accepted).
std::size_t append_facts(const std::string& store, const std::vector<std::string>& facts) {
  std::set<std::string> existing;
  {
    std::ifstream f(store);
    std::string l;
    while (std::getline(f, l)) {
      auto j = nlohmann::json::parse(l, nullptr, false);
      if (j.is_object() && j.contains("text") && j["text"].is_string())
        existing.insert(j["text"].get<std::string>());
    }
  }
  std::ofstream f(store, std::ios::app);
  if (!f) return 0;
  std::size_t n = 0;
  const double ts = std::chrono::duration<double>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
  for (const auto& t : facts) {
    if (existing.count(t)) continue;
    f << nlohmann::json{{"text", t}, {"ts", ts}, {"src", "auto"}}.dump() << "\n";
    ++n;
  }
  return n;
}
}  // namespace

void AutoExtractModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("store") && !cfg.kv.at("store").empty()) store_ = cfg.kv.at("store");
  if (cfg.kv.count("model")) model_ = cfg.kv.at("model");
  if (cfg.kv.count("price_per_mtok"))
    set_pos_double_on_string(cfg.kv.at("price_per_mtok"), price_per_mtok_);
  if (cfg.kv.count("max_facts")) {
    try {
      const long n = std::stol(cfg.kv.at("max_facts"));
      if (n > 0) max_facts_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default */ }
  }
  if (provider_) return;  // injected (tests)
  double timeout_s = 60.0;
  if (cfg.kv.count("timeout_s")) set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s);
  const std::string ep  = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
  const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("auto_extract: api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model_, cpr_http(timeout_s));
}

void AutoExtractModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("TURN_ORIGIN", [this](const Entry& e) {
    if (e.value.is_string()) origin_ = e.value.get<std::string>();
  });
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (e.value.is_string()) last_user_ = e.value.get<std::string>();
  });
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    // Gate on the pump thread; the review itself runs on a worker (or inline w/o executor).
    if (!e.value.is_string()) return;
    const std::string assistant = e.value.get<std::string>();
    if (origin_ != "human") return;                       // never harvest peer/heartbeat turns
    if (last_user_.empty() || assistant.empty()) return;
    if (is_turn_artifact(assistant)) return;
    if (busy_.exchange(true)) return;                     // one review in flight; skip, not queue
    // Capture discipline (LLMModule precedent): non-owning provider/bus pointers + plain
    // values + the atomic flag. `this` fields are NOT touched by the worker.
    Provider* prov = provider_.get();
    Blackboard* bus = bb_;
    std::atomic<bool>* busy = &busy_;
    const std::string store = store_;
    const std::string model = model_;
    const double price = price_per_mtok_;
    const std::size_t max_facts = max_facts_;
    LlmRequest req;
    req.model = model;
    req.messages = {nlohmann::json{{"role", "system"}, {"content", kSystemPrompt}},
                    nlohmann::json{{"role", "user"},
                                   {"content", build_extract_digest(last_user_, assistant)}}};
    auto run = [prov, bus, busy, store, price, max_facts](const LlmRequest& r) {
      try {
        const LlmResponse resp = prov->complete(r);
        const auto facts = parse_extract_reply(resp.text, max_facts);
        if (!facts.empty()) append_facts(store, facts);
        const double delta =
            (static_cast<double>(resp.prompt_tokens) + resp.completion_tokens) / 1e6 * price;
        if (delta > 0.0) bus->post("AUX_SPENT_USD", delta, "auto_extract");
      } catch (...) { /* fail-soft: an extractor error never touches a turn */ }
      busy->store(false);
    };
    if (executor_) executor_->submit([req = std::move(req), run] { run(req); });
    else run(req);
  });
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R AutoExtract` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/auto_extract_module.h src/apps/auto_extract/auto_extract.cpp tests/test_auto_extract_module.cpp CMakeLists.txt
git commit -m "feat: AutoExtractModule — human-turn background review writes archival facts, posts AUX_SPENT_USD"
```

---

## Task 4: Wiring + ship

**Files:**
- Modify: `app/agent_wiring.h` (include + `Agent.auto_extract` member after `status`), `app/agent_wiring.cpp` (factory, take_as, merged cfg, `set_executor`, attach)
- Test: `tests/test_auto_extract_wiring.cpp`
- Modify: `CMakeLists.txt`, `docs/manifest-reference.md`, `manifests/dev.hades`, `CLAUDE.md`

**Interfaces:**
- Consumes: `AutoExtractModule` (T3); the Session block and the already-resolved `store_path` in `wire_agent`; the Executor created before `wire_agent` on the Manifest path.
- Produces: `Module = auto_extract` roster name; optional `AutoExtract` manifest block (`model`, `max_facts`, `timeout_s`); merged-cfg convention documented.

- [ ] **Step 1: Failing wiring test** `tests/test_auto_extract_wiring.cpp`:

```cpp
// tests/test_auto_extract_wiring.cpp — manifest-path wiring for Module = auto_extract
#include <gtest/gtest.h>
#include <cstdlib>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(AutoExtractWiring, RosteredModuleIsBuiltAndAbsentIsNull) {
  ::setenv("HADES_API_KEY", "dummy", 1);
  Blackboard bb;
  Manifest with = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = auto_extract\nModule = arbiter\n"
      "AutoExtract\n{\n  model = cheap-model\n  max_facts = 2\n}\n");
  Agent a = build_agent(bb, with);
  EXPECT_NE(a.auto_extract, nullptr);
  Blackboard bb2;
  Manifest without = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent b = build_agent(bb2, without);
  EXPECT_EQ(b.auto_extract, nullptr);
}
```

CMake: `target_sources(hades_tests PRIVATE tests/test_auto_extract_wiring.cpp)`. Run → compile FAIL (`Agent` has no member `auto_extract`).

- [ ] **Step 2: Implement wiring.** In `app/agent_wiring.h`: `#include "hades/module/auto_extract_module.h"`; member after `status`:

```cpp
  std::unique_ptr<AutoExtractModule> auto_extract;  // optional background memory harvest
```

In `app/agent_wiring.cpp`:
1. Factory + take_as beside the others: `launcher.register_factory("auto_extract", []{ return std::make_unique<AutoExtractModule>(); });` and `a.auto_extract = take_as<AutoExtractModule>(launcher, "auto_extract");`
2. Extract the block in the Manifest overload beside the other blocks: `const auto ae_blocks = m.of("AutoExtract"); const Block ae_cfg = ae_blocks.empty() ? Block{} : ae_blocks.front();` and pass it to `wire_agent` as a new trailing defaulted parameter `const Block& auto_extract_cfg = Block{}` (after `skills_cfg` — check the current trailing params and keep their order; update the one Manifest-overload call site).
3. In `wire_agent`, after the SkillsModule/status attach section (2e), add:

```cpp
  // 2f) AutoExtractModule: merged cfg = Session transport values + AutoExtract overrides +
  //     the already-resolved archival store path (single source of truth with save_memory).
  if (a.auto_extract) {
    Block merged = auto_extract_cfg;                     // model / max_facts / timeout_s
    auto inherit = [&](const char* k) {
      if (!merged.kv.count(k) && session.kv.count(k)) merged.kv[k] = session.kv.at(k);
    };
    inherit("endpoint");
    inherit("api_key_env");
    inherit("price_per_mtok");
    if (!merged.kv.count("model") && session.kv.count("model"))
      merged.kv["model"] = session.kv.at("model");       // aux default = the main model
    merged.kv["store"] = store_path;
    if (executor) a.auto_extract->set_executor(executor);
    a.auto_extract->on_start(merged, bb);
    a.auto_extract->on_attach(bb);
  }
```

(Use the actual Session-block variable name in `wire_agent` — it is `session` — and the actual executor variable/parameter used for the LLM/embedding `set_executor` calls; mirror how `a.embedding` gets its executor. If `wire_agent` receives the executor only via the LLM path, mirror the embedding module's mechanism exactly.)

- [ ] **Step 3: Build + test.** `-R AutoExtract` → all pass; **full suite** green. Also run the TSan lane: `nix develop --command bash -c 'cmake --build build-tsan && ctest --test-dir build-tsan'` → green (the module tests run inline — no executor — but the LLMModule fold and any offload interplay get the lane's coverage).
- [ ] **Step 4: Ship docs.**
  - `manifests/dev.hades` — after the `# Module = status` line region (keep the template's commented-optional style), add:

```
# --- Auto-extract: after each human turn, a background LLM call harvests durable facts
# (preferences, corrections, standing facts) into archival memory with src:"auto". Costs one
# extra small LLM call per turn, metered into the budget. The AutoExtract block is optional;
# model defaults to the Session model (point it at a cheaper one if your provider has it).
# Module = auto_extract
# AutoExtract
# {
#   model     = gpt-5.5
#   max_facts = 3
#   timeout_s = 60
# }
```

  - `docs/manifest-reference.md`: §2 roster row (`auto_extract` — what it does / omitting it: no automatic harvesting); new numbered section (after §17) documenting the block keys table (`model` default Session.model · `max_facts` 3 · `timeout_s` 60), the merged-cfg inheritance (endpoint/api_key_env/price from Session), the `AUX_SPENT_USD` → budget fold, human-origin-only + artifact gates, skip-if-busy, `src:"auto"` provenance, dedup, and the prompt-injection caveat (bounded blast radius). Bus-key note: add `AUX_SPENT_USD` wherever bus keys are enumerated (§15's bus-keys table has the pattern).
  - `CLAUDE.md`: feature subsection (mechanics, capture discipline, budget fold, gates, config), memory-v2 work-list item 2 marked SHIPPED (auto-extract half; note prompt-injection caveat + semantic-dedup v2), current-state test count updated.
- [ ] **Step 5: Full suite (both lanes) + commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp tests/test_auto_extract_wiring.cpp \
  CMakeLists.txt docs/manifest-reference.md manifests/dev.hades CLAUDE.md
git commit -m "feat: wire auto_extract — roster module, merged Session+AutoExtract cfg, ship docs"
```

---

## Verification (end-to-end)

1. Full suite ASan+UBSan AND TSan green (baseline 672 + ~12 new).
2. Manual live smoke (Vaios): roster `Module = auto_extract` in dev.local.hades → chat a preference ("I prefer answers in bullet points") → `.hades/memory.jsonl` gains a `src:"auto"` line within seconds; next session, the preference surfaces in recall. `hades-scope session.log AUX` shows the spend deltas.
3. Budget check: `stay_on_budget` trips earlier with auto_extract on (aux spend metered).

## Execution

Subagent-driven development: fresh opus implementer per task, opus cpp-reviewer per task, final whole-branch review (opus) incl. the TSan lane, then finishing-a-development-branch (ff merge to main; push only on Vaios's word).
