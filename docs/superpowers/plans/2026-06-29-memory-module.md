# MemoryModule Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the hades agent dynamic memory — a `save_memory` tool the LLM calls to persist facts, plus a `MemoryModule` app that retrieves the relevant slice each turn and the Arbiter injects into context.

**Architecture:** Tool writes, app reads, one shared JSONL file store. `save_memory` (native subprocess, append-only) writes records; `MemoryModule` (Blackboard module) subscribes `USER_MESSAGE`, loads the store, ranks by keyword overlap, posts `RETRIEVED_MEMORY`; the Arbiter splices that as an ephemeral `{role:system}` block immediately before the last user message (never stored in `history_`). Keyword ranker is a pure function — the seam embeddings drop into for v2.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell (`nix develop`) · `nlohmann/json` · GoogleTest · POSIX fork/exec (existing `run_subprocess`).

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`** (e.g. `nix develop --command ctest --test-dir build`).
- **Build:** CMake ≥3.20 + Ninja, out-of-tree `build/`. Each task appends sources via `target_sources(... PRIVATE ...)`; new tool binaries are their own `add_executable`.
- **Immutability:** never mutate a caller's object in place; copy-then-modify (e.g. the Tool block list in wiring).
- **Many small files** (~200–400 lines), one responsibility each.
- **Config = plain-text MOOS-style blocks** (`Section = name { key = value }`). New `Memory { }` block.
- **TDD:** RED → GREEN → COMMIT every task. Commit style `<type>: <desc>` (feat/fix/docs); **no attribution footer**.
- **Fail soft on bad data:** missing store file → empty list; malformed JSONL line → skip; never throw on parse.
- **save_memory is append-only to the agent's own store → NOT confirm-gated** (do NOT add it to `AvoidDestructive`'s mutating-tools set).

---

## File Structure

```
include/hades/memory/record.h          T1  MemoryRecord {text, ts}
include/hades/memory/rank.h            T1  rank_memories(all, query, top_n)
src/memory/rank.cpp                    T1
include/hades/memory/store.h           T2  load_memories(path)
src/memory/store.cpp                   T2
tools/save_memory_main.cpp             T3  binary hades-save-memory
include/hades/module/memory_module.h   T4  MemoryModule (type()=="memory")
src/module/memory_module.cpp           T4
src/arbiter/arbiter.cpp                T5  (modify) inject RETRIEVED_MEMORY
app/agent_wiring.{h,cpp}               T6  (modify) build + register MemoryModule
manifests/dev.hades                    T6  (modify) Memory block + save_memory tool
tests/test_memory_rank.cpp             T1
tests/test_memory_store.cpp            T2
tests/test_save_memory_tool.cpp        T3
tests/test_memory_module.cpp           T4
tests/test_arbiter.cpp                 T5  (extend)
tests/test_memory_wiring.cpp           T6
```

New Blackboard key: `RETRIEVED_MEMORY` — string (rendered block; `""` if none); producer MemoryModule → consumer Arbiter.

---

## Task 1: Keyword ranker (pure function) + MemoryRecord

**Files:**
- Create: `include/hades/memory/record.h`, `include/hades/memory/rank.h`, `src/memory/rank.cpp`
- Test: `tests/test_memory_rank.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `struct MemoryRecord { std::string text; double ts = 0.0; };` and
  `std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all, const std::string& query, std::size_t top_n);`

- [ ] **Step 1: Write the failing test** — `tests/test_memory_rank.cpp`:

```cpp
// tests/test_memory_rank.cpp — pure keyword ranker: overlap scoring, recency tie, top_n cap
#include <gtest/gtest.h>
#include "hades/memory/rank.h"
using namespace hades;

TEST(MemoryRank, ScoresOverlapDropsZero) {
  std::vector<MemoryRecord> all = {
      {"the cat sat on the mat", 1.0},
      {"dogs are loyal animals", 2.0},      // no exact token overlap with "cat dog" -> dropped
      {"a cat and a dog played", 3.0},
  };
  auto top = rank_memories(all, "cat dog", 5);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "a cat and a dog played");  // score 2 wins
  EXPECT_EQ(top[1].text, "the cat sat on the mat");  // score 1
}

TEST(MemoryRank, RecencyTieBreakAndTopNCap) {
  std::vector<MemoryRecord> all = {{"cat one", 1.0}, {"cat two", 5.0}, {"cat three", 3.0}};
  auto top = rank_memories(all, "cat", 2);
  ASSERT_EQ(top.size(), 2u);              // capped at top_n
  EXPECT_EQ(top[0].text, "cat two");      // newest first on equal score
  EXPECT_EQ(top[1].text, "cat three");
}

TEST(MemoryRank, EmptyQueryYieldsNothing) {
  std::vector<MemoryRecord> all = {{"cat", 1.0}};
  EXPECT_TRUE(rank_memories(all, "", 5).empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `fatal error: hades/memory/rank.h: No such file`.

- [ ] **Step 3: Write minimal implementation**

`include/hades/memory/record.h`:
```cpp
// include/hades/memory/record.h — one persisted memory: free text + save timestamp
#pragma once
#include <string>
namespace hades {
struct MemoryRecord { std::string text; double ts = 0.0; };
}  // namespace hades
```

`include/hades/memory/rank.h`:
```cpp
// include/hades/memory/rank.h — keyword retrieval over memory records (v1 scorer)
//
// Score each record by the number of distinct query tokens present in its text;
// drop zero-score records; sort by score desc, then ts desc (recency tie-break);
// return at most top_n. Pure: no files, no Blackboard. This is the seam a v2
// embeddings scorer slots into behind the same signature.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {
std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n);
}  // namespace hades
```

`src/memory/rank.cpp`:
```cpp
// src/memory/rank.cpp — v1 keyword ranker (exact lowercased token overlap)
#include "hades/memory/rank.h"
#include <algorithm>
#include <cctype>
#include <set>
namespace hades {

static std::set<std::string> tokenize(const std::string& s) {
  std::set<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!cur.empty()) { out.insert(cur); cur.clear(); }
  }
  if (!cur.empty()) out.insert(cur);
  return out;
}

std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n) {
  const auto q = tokenize(query);
  struct Scored { const MemoryRecord* rec; int score; };
  std::vector<Scored> scored;
  for (const auto& rec : all) {
    const auto t = tokenize(rec.text);
    int score = 0;
    for (const auto& w : q) if (t.count(w)) ++score;
    if (score > 0) scored.push_back({&rec, score});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.rec->ts > b.rec->ts;
  });
  std::vector<MemoryRecord> out;
  for (std::size_t i = 0; i < scored.size() && i < top_n; ++i) out.push_back(*scored[i].rec);
  return out;
}

}  // namespace hades
```

`CMakeLists.txt` — add after line `target_sources(hades_core PRIVATE src/config/prompt.cpp)`:
```cmake
target_sources(hades_core PRIVATE src/memory/rank.cpp)
target_sources(hades_tests PRIVATE tests/test_memory_rank.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R MemoryRank`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add include/hades/memory/record.h include/hades/memory/rank.h src/memory/rank.cpp tests/test_memory_rank.cpp CMakeLists.txt
git commit -m "feat: keyword memory ranker (pure fn) + MemoryRecord"
```

---

## Task 2: MemoryStore loader (JSONL)

**Files:**
- Create: `include/hades/memory/store.h`, `src/memory/store.cpp`
- Test: `tests/test_memory_store.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MemoryRecord` (T1).
- Produces: `std::vector<MemoryRecord> load_memories(const std::string& path);`

- [ ] **Step 1: Write the failing test** — `tests/test_memory_store.cpp`:

```cpp
// tests/test_memory_store.cpp — JSONL store loader: valid lines kept, junk skipped, missing -> empty
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include "hades/memory/store.h"
using namespace hades;

TEST(MemoryStore, LoadsValidSkipsMalformed) {
  const std::string path = ::testing::TempDir() + "/store.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"alpha","ts":1.5})" << "\n";
    f << "not json at all" << "\n";          // malformed -> skip
    f << R"({"ts":2.0})" << "\n";            // missing text -> skip
    f << R"({"text":"beta","ts":3.0})" << "\n";
  }
  auto v = load_memories(path);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].text, "alpha");
  EXPECT_DOUBLE_EQ(v[0].ts, 1.5);
  EXPECT_EQ(v[1].text, "beta");
}

TEST(MemoryStore, MissingFileIsEmpty) {
  EXPECT_TRUE(load_memories(::testing::TempDir() + "/no_such_store.jsonl").empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `hades/memory/store.h: No such file`.

- [ ] **Step 3: Write minimal implementation**

`include/hades/memory/store.h`:
```cpp
// include/hades/memory/store.h — append-only JSONL memory store reader
//
// load_memories: parse one JSON object per line ({"text","ts"}). Missing file ->
// empty (a fresh agent). Malformed or text-less lines are skipped, never thrown.
#pragma once
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {
std::vector<MemoryRecord> load_memories(const std::string& path);
}  // namespace hades
```

`src/memory/store.cpp`:
```cpp
// src/memory/store.cpp — read the JSONL memory store, tolerant of junk lines
#include "hades/memory/store.h"
#include <fstream>
#include <nlohmann/json.hpp>
namespace hades {

std::vector<MemoryRecord> load_memories(const std::string& path) {
  std::vector<MemoryRecord> out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh agent, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("text") || !j["text"].is_string())
      continue;  // skip malformed / text-less records
    out.push_back({j["text"].get<std::string>(), j.value("ts", 0.0)});
  }
  return out;
}

}  // namespace hades
```

`CMakeLists.txt` — add after the Task 1 lines:
```cmake
target_sources(hades_core PRIVATE src/memory/store.cpp)
target_sources(hades_tests PRIVATE tests/test_memory_store.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R MemoryStore`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add include/hades/memory/store.h src/memory/store.cpp tests/test_memory_store.cpp CMakeLists.txt
git commit -m "feat: JSONL memory store loader (skip-bad, missing-ok)"
```

---

## Task 3: `save_memory` native tool binary

**Files:**
- Create: `tools/save_memory_main.cpp` (→ binary `hades-save-memory`)
- Test: `tests/test_save_memory_tool.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: binary `hades-save-memory`. Protocol (one JSON line in/out, like the other tools):
  `{"call":"describe"}` → `{"ok":true,"result":{"name":"save_memory","description":...,"schema":{...,"required":["text"]}}}`;
  `{"call":"save_memory","args":{"text":"..."}}` → appends `{"text","ts"}` to `argv[1]` (default `.hades/memory.jsonl`), returns `{"ok":true,"result":{"saved":true}}`.
- Consumes: `hades::run_subprocess` (existing) in the test.

- [ ] **Step 1: Write the failing test** — `tests/test_save_memory_tool.cpp`:

```cpp
// tests/test_save_memory_tool.cpp — drive the hades-save-memory binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

TEST(SaveMemoryTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_memory");
  EXPECT_TRUE(j["result"].contains("schema"));
}

TEST(SaveMemoryTool, AppendsRecordLine) {
  const std::string store = ::testing::TempDir() + "/save_tool.jsonl";
  std::remove(store.c_str());
  nlohmann::json call{{"call", "save_memory"}, {"args", {{"text", "remember this"}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  std::ifstream f(store);
  std::string line;
  std::getline(f, line);
  auto rec = nlohmann::json::parse(line, nullptr, false);
  EXPECT_EQ(rec.value("text", ""), "remember this");
  EXPECT_TRUE(rec.contains("ts"));
}

TEST(SaveMemoryTool, MissingTextIsNotOk) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, ::testing::TempDir() + "/x.jsonl"},
                                R"({"call":"save_memory","args":{}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  EXPECT_FALSE(j.value("ok", true));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `SAVE_MEMORY_BIN` undefined / target `hades-save-memory` does not exist.

- [ ] **Step 3: Write minimal implementation**

`tools/save_memory_main.cpp`:
```cpp
// tools/save_memory_main.cpp — bundled save_memory native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_memory","args":{text}}), APPENDS one
// record line {"text","ts"} to the memory store, and writes one JSON line. Store path =
// argv[1] (fallback ".hades/memory.jsonl"). Append-only to the agent's own store; NOT
// confirm-gated (unlike write_file). Speaks the hades one-JSON-line native tool protocol;
// all stdin parsing is guarded so a malformed request never throws.
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/memory.jsonl";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "save_memory"},
             {"description", "Persist a fact or observation to long-term memory."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"text", {{"type", "string"}}}}},
               {"required", {"text"}}}}}}};
  } else if (call == "save_memory") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    std::string text = args.value("text", "");
    if (text.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      double ts = std::chrono::duration<double>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
      std::ofstream f(store, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + store}}}};
      } else {
        f << nlohmann::json{{"text", text}, {"ts", ts}}.dump() << "\n";
        out = {{"ok", true}, {"result", {{"saved", true}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
```

`CMakeLists.txt` — add the executable after the `hades-http-fetch` block (around line 45):
```cmake
add_executable(hades-save-memory tools/save_memory_main.cpp)
target_link_libraries(hades-save-memory PRIVATE nlohmann_json::nlohmann_json)
```
And register the test + binary path. Add after the Task 2 lines:
```cmake
target_sources(hades_tests PRIVATE tests/test_save_memory_tool.cpp)
target_compile_definitions(hades_tests PRIVATE SAVE_MEMORY_BIN="$<TARGET_FILE:hades-save-memory>")
add_dependencies(hades_tests hades-save-memory)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SaveMemoryTool`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/save_memory_main.cpp tests/test_save_memory_tool.cpp CMakeLists.txt
git commit -m "feat: save_memory native tool (append-only JSONL store)"
```

---

## Task 4: `MemoryModule` (retrieval app)

**Files:**
- Create: `include/hades/module/memory_module.h`, `src/module/memory_module.cpp`
- Test: `tests/test_memory_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `load_memories` (T2), `rank_memories` (T1), `Module`/`Block`/`Blackboard`/`Entry` (existing).
- Produces: `class MemoryModule : public Module` with `type()=="memory"`, `on_start(const Block&, Blackboard&)`, `on_attach(Blackboard&)`. Posts `RETRIEVED_MEMORY` (string) on each `USER_MESSAGE`.

- [ ] **Step 1: Write the failing test** — `tests/test_memory_module.cpp`:

```cpp
// tests/test_memory_module.cpp — USER_MESSAGE -> load store, rank, post RETRIEVED_MEMORY
#include <gtest/gtest.h>
#include <fstream>
#include "hades/module/memory_module.h"
#include "hades/blackboard.h"
using namespace hades;

TEST(MemoryModule, PostsMatchingMemoryExcludesNonMatching) {
  const std::string path = ::testing::TempDir() + "/mm.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"user prefers tea over coffee","ts":1.0})" << "\n";
    f << R"({"text":"project deadline is friday","ts":2.0})" << "\n";
  }
  Blackboard bb;
  MemoryModule m;
  Block cfg;
  cfg.kv["store"] = path;
  cfg.kv["top_n"] = "5";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "what tea do i like", "chat");
  bb.pump();
  EXPECT_NE(got.find("tea"), std::string::npos);
  EXPECT_EQ(got.find("deadline"), std::string::npos);  // non-matching record excluded
}

TEST(MemoryModule, EmptyStringWhenNoMatchOrNoStore) {
  Blackboard bb;
  MemoryModule m;
  Block cfg;
  cfg.kv["store"] = ::testing::TempDir() + "/mm_absent.jsonl";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "anything", "chat");
  bb.pump();
  EXPECT_EQ(got, "");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `hades/module/memory_module.h: No such file`.

- [ ] **Step 3: Write minimal implementation**

`include/hades/module/memory_module.h`:
```cpp
// include/hades/module/memory_module.h — dynamic memory retrieval app (MOOS-app style)
//
// Subscribes USER_MESSAGE; each turn it loads the JSONL store, ranks records by keyword
// overlap with the message, renders the top_n as a bullet block, and posts
// RETRIEVED_MEMORY for the Arbiter to inject. Save is handled separately by the
// save_memory tool (this module is read-only over the store). Empty result -> "".
#pragma once
#include <cstddef>
#include <string>
#include "hades/module.h"
namespace hades {
class Blackboard;
class MemoryModule : public Module {
public:
  std::string type() const override { return "memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

private:
  std::string store_path_ = ".hades/memory.jsonl";
  std::size_t top_n_ = 5;
  Blackboard* bb_ = nullptr;
};
}  // namespace hades
```

`src/module/memory_module.cpp` (lives under `src/module/` to match the other modules):
```cpp
// src/module/memory_module.cpp — load store, rank vs user message, post RETRIEVED_MEMORY
#include "hades/module/memory_module.h"
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/memory/rank.h"
#include "hades/memory/store.h"
namespace hades {

void MemoryModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("store")) store_path_ = cfg.kv.at("store");
  if (cfg.kv.count("top_n")) {
    try {
      long n = std::stol(cfg.kv.at("top_n"));
      if (n > 0) top_n_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default on garbage */ }
  }
}

void MemoryModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string()) return;  // ignore malformed input
    const auto all = load_memories(store_path_);
    const auto top = rank_memories(all, e.value.get<std::string>(), top_n_);
    std::string block;
    for (const auto& r : top) block += "- " + r.text + "\n";
    if (!block.empty() && block.back() == '\n') block.pop_back();
    bb_->post("RETRIEVED_MEMORY", block, "memory");  // "" when nothing matched
  });
}

}  // namespace hades
```

`CMakeLists.txt` — add after the Task 3 lines:
```cmake
target_sources(hades_core PRIVATE src/module/memory_module.cpp)
target_sources(hades_tests PRIVATE tests/test_memory_module.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R MemoryModule`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add include/hades/module/memory_module.h src/module/memory_module.cpp tests/test_memory_module.cpp CMakeLists.txt
git commit -m "feat: MemoryModule — keyword retrieval app posts RETRIEVED_MEMORY"
```

---

## Task 5: Arbiter injects retrieved memory

**Files:**
- Modify: `src/arbiter/arbiter.cpp` (`start_turn`, lines 31–44)
- Test: `tests/test_arbiter.cpp` (extend)

**Interfaces:**
- Consumes: latest `RETRIEVED_MEMORY` from the Blackboard via `bb_->get("RETRIEVED_MEMORY")`.
- Produces: when non-empty, an ephemeral `{role:system, content:"Relevant memories:\n"+mem}` message spliced immediately before the last `user` message in `LLM_REQUEST.messages`. Not stored in `history_`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (before the final `}` of the file is not needed — these are top-level TESTs; add at end of file):

```cpp
TEST(Arbiter, InjectsRetrievedMemoryBeforeUserMessage) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("RETRIEVED_MEMORY","- user likes tea","memory");
  bb.post("USER_MESSAGE","what do i like","chat"); bb.pump();
  const auto& msgs = req["messages"];
  int memIdx=-1, userIdx=-1;
  for (int i=0;i<static_cast<int>(msgs.size());++i){
    if (msgs[i].value("role","")=="system" &&
        msgs[i].value("content","").rfind("Relevant memories:",0)==0) memIdx=i;
    if (msgs[i].value("role","")=="user") userIdx=i;
  }
  ASSERT_GE(memIdx,0); ASSERT_GE(userIdx,0);
  EXPECT_LT(memIdx,userIdx);                                  // memory block precedes the user turn
  EXPECT_NE(msgs[memIdx]["content"].get<std::string>().find("tea"), std::string::npos);
}

TEST(Arbiter, NoMemoryBlockWhenRetrievedEmpty) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("RETRIEVED_MEMORY","","memory");
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  for (const auto& m : req["messages"])
    EXPECT_FALSE(m.value("role","")=="system" &&
                 m.value("content","").rfind("Relevant memories:",0)==0);
}

TEST(Arbiter, MemoryBlockIsEphemeralNotInHistory) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY","- ephemeral fact","memory");
  bb.post("USER_MESSAGE","first","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","ok"}}, "llm"); bb.pump();   // turn 1 ends
  bb.post("RETRIEVED_MEMORY","","memory");                       // nothing relevant now
  bb.post("USER_MESSAGE","second","chat"); bb.pump();           // turn 2
  bool leaked=false;
  for (const auto& m : reqs.back()["messages"])
    if (m.value("role","")=="system" && m.value("content","").rfind("Relevant memories:",0)==0)
      leaked=true;
  EXPECT_FALSE(leaked);   // prior turn's memory block did not persist into history_
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Arbiter`
Expected: FAIL — `InjectsRetrievedMemoryBeforeUserMessage` finds no memory block (`memIdx` stays -1).

- [ ] **Step 3: Write minimal implementation** — replace `Arbiter::start_turn()` in `src/arbiter/arbiter.cpp` (current lines 31–44) with:

```cpp
void Arbiter::start_turn() {
  nlohmann::json tools = nlohmann::json::array();
  for (auto& t : tools_)
    tools.push_back({{"name", t.name}, {"description", t.description}, {"schema", t.schema}});
  // Prepend the assembled system prompt (if any) as messages[0]; the conversation
  // history follows. system_prompt_ is NOT stored in history_ so it stays exactly one
  // leading message and never duplicates across turns.
  nlohmann::json messages = nlohmann::json::array();
  if (!system_prompt_.empty())
    messages.push_back({{"role", "system"}, {"content", system_prompt_}});
  for (const auto& m : history_) messages.push_back(m);

  // Inject dynamically retrieved memory as an ephemeral {role:system} block immediately
  // before the last user message. Recomputed from the Blackboard each turn and never
  // stored in history_, so it refreshes per turn and never accumulates stale memory.
  if (auto mem = bb_->get("RETRIEVED_MEMORY");
      mem && mem->value.is_string() && !mem->value.get<std::string>().empty()) {
    nlohmann::json block = {{"role", "system"},
                            {"content", "Relevant memories:\n" + mem->value.get<std::string>()}};
    int last_user = -1;
    for (int i = 0; i < static_cast<int>(messages.size()); ++i)
      if (messages[i].value("role", "") == "user") last_user = i;
    if (last_user >= 0) messages.insert(messages.begin() + last_user, block);
  }

  bb_->post("LLM_REQUEST",
            {{"messages", messages}, {"tools", tools}, {"model", model_}}, "arbiter");
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Arbiter`
Expected: PASS (all Arbiter tests, including the 3 new).

- [ ] **Step 5: Commit**

```bash
git add src/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter injects RETRIEVED_MEMORY as ephemeral block before user msg"
```

---

## Task 6: Wire MemoryModule into the agent graph + manifest + integration test

**Files:**
- Modify: `app/agent_wiring.h` (Agent struct + test overload signature), `app/agent_wiring.cpp` (build + register)
- Modify: `manifests/dev.hades`
- Test: `tests/test_memory_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MemoryModule` (T4), `hades-save-memory` (T3), existing `build_agent` plumbing.
- Produces: `Agent` now owns `std::unique_ptr<MemoryModule> memory`, attached BEFORE the Arbiter. The `build_agent` test overload gains a trailing `const Block& memory = Block{}` param. The save_memory tool's `native` command gets the configured store path appended (single source of truth = the `Memory` block).

- [ ] **Step 1: Write the failing test** — `tests/test_memory_wiring.cpp`:

```cpp
// tests/test_memory_wiring.cpp — build_agent wires MemoryModule (before Arbiter) and the
// save_memory tool path; a seeded store surfaces through the live graph as RETRIEVED_MEMORY.
#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
struct AnswerProvider : Provider {  // minimal: always a plain answer, no tool calls
  LlmResponse complete(const LlmRequest&) override { LlmResponse r; r.text = "ok"; return r; }
};
}  // namespace

TEST(MemoryWiring, MemoryAttachedAndSeededStoreSurfaces) {
  const std::string store = ::testing::TempDir() + "/wire.jsonl";
  { std::ofstream f(store); f << R"({"text":"vaios likes lightning network","ts":1.0})" << "\n"; }

  Blackboard bb;
  std::vector<Block> tools;  // a save_memory Tool block with the bare binary (no path)
  Block t; t.section = "Tool"; t.name = "save_memory"; t.kv["native"] = SAVE_MEMORY_BIN;
  tools.push_back(t);
  Block mem; mem.section = "Memory"; mem.kv["store"] = store; mem.kv["top_n"] = "5";

  Agent agent = build_agent(bb, std::make_unique<AnswerProvider>(), tools, {}, "m", mem);
  ASSERT_TRUE(agent.memory != nullptr);

  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about lightning", "chat");
  bb.pump();
  EXPECT_NE(got.find("lightning"), std::string::npos);  // retrieval ran through the built graph
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `build_agent` has no 6-arg overload; `Agent` has no `memory` member.

- [ ] **Step 3: Write minimal implementation**

In `app/agent_wiring.h`: add the include and the struct member, and the trailing default param on the test overload.

Add to the includes (after `#include "hades/module/http_server_module.h"`):
```cpp
#include "hades/module/memory_module.h"
```

Add to the `Agent` struct (after the `arbiter` member):
```cpp
  std::unique_ptr<MemoryModule> memory;
```

Change the test `build_agent` overload declaration to:
```cpp
Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model,
                  const Block& memory = Block{});
```

In `app/agent_wiring.cpp`:

Add the include (after `#include "hades/prompt.h"`):
```cpp
#include "hades/module/memory_module.h"
```

Change `build_agent_impl`'s signature to take the memory block (add a `const Block& memory` parameter after `objectives`):
```cpp
Agent build_agent_impl(Blackboard& bb,
                       const Block& session,
                       std::unique_ptr<Provider> llm_provider,
                       const std::vector<Block>& tools,
                       const std::vector<Block>& objectives,
                       const Block& memory,
                       std::string model) {
```

At the top of `build_agent_impl` body, before constructing modules, derive the store path and rewrite the save_memory tool command (immutability: build a new list):
```cpp
  // Single source of truth: the save_memory tool writes to the same store the
  // MemoryModule reads. Append the configured store path to the save_memory tool's
  // command so the two cannot drift. Copy-then-modify; never touch the caller's blocks.
  const std::string store_path =
      memory.kv.count("store") ? memory.kv.at("store") : ".hades/memory.jsonl";
  std::vector<Block> tools_resolved;
  tools_resolved.reserve(tools.size());
  for (Block t : tools) {
    if (t.name == "save_memory" && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + store_path;
    tools_resolved.push_back(std::move(t));
  }
```
Then change the tool-loading loop to use `tools_resolved` instead of `tools`:
```cpp
  for (const auto& t : tools_resolved) a.tools->add_tool(t);
```
Add the MemoryModule to the holder (after `a.serve = std::make_unique<HttpServerModule>();`):
```cpp
  a.memory  = std::make_unique<MemoryModule>();
```
Register the MemoryModule BEFORE the Arbiter. Insert this block immediately before the `// 3) Arbiter:` comment:
```cpp
  // 2b) MemoryModule BEFORE the Arbiter: on a USER_MESSAGE the pump dispatches in
  //     registration order, so the module posts RETRIEVED_MEMORY (latest-value updated
  //     synchronously) before the Arbiter's start_turn() reads it the same turn.
  a.memory->on_start(memory, bb);
  a.memory->on_attach(bb);
```

Update the two public overloads to pass the memory block:
```cpp
Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model,
                  const Block& memory) {
  return build_agent_impl(bb, Block{}, std::move(llm), tools, objectives, memory, std::move(model));
}

Agent build_agent(Blackboard& bb, const Manifest& m) {
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const Block& s = *session;

  const std::string endpoint = s.kv.count("endpoint") ? s.kv.at("endpoint") : "";
  const std::string model    = s.kv.count("model")    ? s.kv.at("model")    : "";
  const std::string env      = s.kv.count("api_key_env") ? s.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("LLM api key env var not set: " + env);

  const auto mem_blocks = m.of("Memory");
  const Block memory = mem_blocks.empty() ? Block{} : mem_blocks.front();

  auto provider = std::make_unique<OpenAICompatProvider>(endpoint, key, model, cpr_http());
  return build_agent_impl(bb, s, std::move(provider), m.of("Tool"), m.of("Objective"), memory, model);
}
```

`manifests/dev.hades` — add a Memory block and the save_memory tool (bare binary; wiring appends the path). Add near the other Tool lines:
```
Memory {
  store = .hades/memory.jsonl
  top_n = 5
}
Tool = save_memory { native = ./build/hades-save-memory }
```

`CMakeLists.txt` — register the wiring test + ensure the binary builds first. Add after the Task 4 lines:
```cmake
target_sources(hades_tests PRIVATE tests/test_memory_wiring.cpp)
```
(`SAVE_MEMORY_BIN` def + `add_dependencies(hades_tests hades-save-memory)` already added in Task 3 cover this test too.)

- [ ] **Step 4: Run the full suite**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`
Expected: all green (the prior 64 + the new memory tests).

- [ ] **Step 5: Commit**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp manifests/dev.hades tests/test_memory_wiring.cpp CMakeLists.txt
git commit -m "feat: wire MemoryModule into agent graph + dev.hades (save_memory tool + Memory block)"
```

---

## Self-Review (against the spec)

- **Spec coverage:** save_memory tool (T3) · MemoryStore (T2) · keyword ranker (T1) · MemoryModule (T4) · Arbiter separate ephemeral block before user msg (T5) · wiring/ordering/one-source-of-truth path + Memory block + manifest (T6). RETRIEVED_MEMORY key (T4/T5). save_memory NOT confirm-gated (T3 — no change to AvoidDestructive). All spec sections mapped.
- **Out of scope honored:** no embeddings/auto-extract/dedup/tags/sqlite/edit-delete/MCP — none introduced.
- **Type consistency:** `MemoryRecord{text,ts}`, `load_memories(path)`, `rank_memories(all,query,top_n)`, `RETRIEVED_MEMORY` string, `MemoryModule::on_start/on_attach`, `build_agent(...,const Block& memory=Block{})` consistent across tasks.
- **Ordering correctness:** MemoryModule attached before Arbiter (T6) so latest-value `RETRIEVED_MEMORY` is fresh when `start_turn()` reads it (single-threaded pump dispatches in subscription order).
- **No placeholders:** every code + test step is complete and copy-pasteable.

## Verification (end-to-end)

1. `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → all green (offline; provider faked).
2. Manual live smoke (needs key):
```bash
export HADES_API_KEY=...
nix develop --command ./build/hades manifests/dev.hades
# user> remember that I prefer the metric system, save it to memory
# user> /quit
nix develop --command ./build/hades-scope session.log RETRIEVED_MEMORY   # see retrieval per turn
cat .hades/memory.jsonl                                                  # see the persisted record
```
3. Restart the agent, ask something touching the saved fact → `hades-scope session.log RETRIEVED_MEMORY` shows the memory surfaced on the matching turn.

## Execution Handoff

Plan saved. Two execution options:
1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks.
2. **Inline Execution** — execute tasks in this session with checkpoints.
