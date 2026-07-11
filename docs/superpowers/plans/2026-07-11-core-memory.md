# core_memory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the append-only `pin_fact` tool with `core_memory` — a bounded, editable core-memory tool (add/replace/remove, char cap) whose over-cap error lists every entry so the model consolidates in the same turn.

**Architecture:** One new native subprocess tool binary (`hades-core-memory`, house one-JSON-line protocol) operating line-wise on the existing `memory/facts.md`; wiring passes the file path + cap via argv (single source of truth); the Arbiter's every-turn fold of `memory_file` is untouched. `pin_fact` (binary, tests, wiring, capability row, docs) is retired.

**Tech Stack:** C++20, CMake+Ninja inside `nix develop`, nlohmann_json, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-07-11-core-memory-design.md` (approved, committed `9b5a0c4`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline **558/558 green** before Task 1; the full suite gates every task.
- Branch `feat/core-memory` (already created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- Tool name is exactly **`core_memory`**; binary target is exactly **`hades-core-memory`**; default cap is exactly **2400** chars, defined ONCE in `include/hades/memory_limit.h` as `inline constexpr long long kDefaultMemoryCharLimit = 2400;` and included by BOTH the tool binary and the wiring (tools do not link hades_core).
- Entries are LINES: every non-empty line of the memory file is an entry; `add`/`replace` write canonical `- <text>` bullet lines; `\n`/`\r` in `text` fold to spaces.
- Empty-string args = absent (the `833b9aa` exactly-one-of lesson): `text`/`match` presence is checked with `.empty()`, never `contains()`.
- All writes atomic (temp file + rename). Fail-closed: malformed/adversarial input → `ok:false`, never throws, never partial-writes.
- **NEVER stage `memory/facts.md`.** `manifests/dev.hades` and `manifests/pi.hades` carry the user's intentional uncommitted edits — Task 3 uses the backup → checkout-clean → edit → commit → restore procedure spelled there; do NOT commit the user's uncommitted manifest values.

---

## File Structure

```
include/hades/memory_limit.h            T1  default cap constant (header-only)
tools/core_memory_main.cpp              T1  the tool binary
tests/test_core_memory_tool.cpp         T1
CMakeLists.txt                          T1 add target; T2 remove pin-fact rows
app/agent_wiring.cpp                    T2  MalConfig check, cap parse, argv append (modify)
src/behaviors/capability_policy.cpp     T2  capability row (modify)
tests/test_capability_policy.cpp        T2  row updates (modify)
tests/test_core_memory_wiring.cpp       T2  (replaces test_pin_fact_wiring.cpp)
tools/pin_fact_main.cpp                 T2  DELETE
tests/test_pin_fact_tool.cpp            T2  DELETE
tests/test_pin_fact_wiring.cpp          T2  DELETE
package.nix                             T2  bins row swap
manifests/dev.hades, manifests/pi.hades T3  Tool line swap (backup/restore procedure)
prompts/soul.md                         T3  memory section rewrite
docs/manifest-reference.md              T3  Session key + tool/argv/capability rows
CLAUDE.md                               T3  feature section + counts
```

---

## Task 1: `core_memory` native tool binary

**Files:**
- Create: `include/hades/memory_limit.h`, `tools/core_memory_main.cpp`
- Test: `tests/test_core_memory_tool.cpp`
- Modify: `CMakeLists.txt` (add target + test rows; pin-fact rows stay for now — suite must remain green mid-branch)

**Interfaces:**
- Consumes: `hades::kDefaultMemoryCharLimit` (created here); `hades/tool/subprocess.h` `run_subprocess` (tests only).
- Produces: binary `hades-core-memory`. Protocol: `{"call":"describe"|"core_memory","args":{action,text,match}}` → one JSON line. argv: `[1]` memory file (fallback `memory/facts.md`), `[2]` char cap (fallback/garbage/`<=0` → 2400). Success result: `{"action":<a>,"entries":N,"chars":N,"cap":N}`. Task 2 relies on the tool name `core_memory`, the argv order `<file> <cap>`, and the over-cap error beginning `"core memory full"`.

- [ ] **Step 1: Write the failing tests** `tests/test_core_memory_tool.cpp`:

```cpp
// tests/test_core_memory_tool.cpp — drive the hades-core-memory binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_file(const char* tag) {
  const std::string f =
      ::testing::TempDir() + "/cm_" + tag + "_" + std::to_string(::getpid()) + ".md";
  std::remove(f.c_str());
  return f;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  return std::string((std::istreambuf_iterator<char>(f)), {});
}
static nlohmann::json call_tool(const std::vector<std::string>& argv, const nlohmann::json& args) {
  nlohmann::json c{{"call", "core_memory"}, {"args", args}};
  ProcResult r = run_subprocess(argv, c.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(CoreMemoryTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({CORE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "core_memory");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "action") != required.end());
}

TEST(CoreMemoryTool, AddAppendsBulletCreatesDirAndReportsUsage) {
  const std::string dir = ::testing::TempDir() + "/cm_dir_" + std::to_string(::getpid());
  fs::remove_all(dir);
  const std::string file = dir + "/facts.md";   // parent dir does not exist yet
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "user is based in Greece"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- user is based in Greece\n");
  EXPECT_EQ(j["result"].value("action", ""), "add");
  EXPECT_EQ(j["result"].value("entries", 0), 1);
  EXPECT_EQ(j["result"].value("cap", 0), 2400);           // default cap reported
  EXPECT_GT(j["result"].value("chars", 0), 0);
}

TEST(CoreMemoryTool, AddDuplicateRejected) {
  const std::string file = fresh_file("dup");
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "same"}}).value("ok", false));
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "same"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("already pinned"), std::string::npos);
  EXPECT_EQ(slurp(file), "- same\n");                     // still exactly one line
}

TEST(CoreMemoryTool, AddOverflowListsEntriesAndWritesNothing) {
  const std::string file = fresh_file("cap");
  // cap 30: first short add fits, second would exceed -> the consolidation error.
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file, "30"}, {{"action", "add"}, {"text", "first fact"}})
                  .value("ok", false));
  const std::string before = slurp(file);
  auto j = call_tool({CORE_MEMORY_BIN, file, "30"},
                     {{"action", "add"}, {"text", "second fact that is far too long for the cap"}});
  EXPECT_FALSE(j.value("ok", true));
  const std::string e = j["result"].value("error", "");
  EXPECT_NE(e.find("core memory full"), std::string::npos);
  EXPECT_NE(e.find("/30 chars"), std::string::npos);      // usage vs cap shown
  EXPECT_NE(e.find("1. - first fact"), std::string::npos);// numbered entry list
  EXPECT_NE(e.find("replace/remove"), std::string::npos); // consolidation instruction
  EXPECT_EQ(slurp(file), before);                          // nothing written
}

TEST(CoreMemoryTool, ReplaceSingleMatchRewritesLine) {
  const std::string file = fresh_file("rep");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "user lives in Athens"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "likes coffee"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "Athens"}, {"text", "user lives in Patras"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- user lives in Patras\n- likes coffee\n");
}

TEST(CoreMemoryTool, ReplaceNoMatchFails) {
  const std::string file = fresh_file("rep0");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "something"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "ghost"}, {"text", "x"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("no entry matches"), std::string::npos);
}

TEST(CoreMemoryTool, ReplaceAmbiguousFailsListingMatches) {
  const std::string file = fresh_file("repN");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "coffee in the morning"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "coffee after lunch"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "coffee"}, {"text", "tea"}});
  EXPECT_FALSE(j.value("ok", true));
  const std::string e = j["result"].value("error", "");
  EXPECT_NE(e.find("ambiguous"), std::string::npos);
  EXPECT_NE(e.find("coffee in the morning"), std::string::npos);   // both candidates listed
  EXPECT_NE(e.find("coffee after lunch"), std::string::npos);
  EXPECT_EQ(slurp(file), "- coffee in the morning\n- coffee after lunch\n");   // untouched
}

TEST(CoreMemoryTool, ReplaceOverflowFailsAndWritesNothing) {
  const std::string file = fresh_file("repcap");
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file, "30"}, {{"action", "add"}, {"text", "short"}})
                  .value("ok", false));
  const std::string before = slurp(file);
  auto j = call_tool({CORE_MEMORY_BIN, file, "30"},
                     {{"action", "replace"}, {"match", "short"},
                      {"text", "a replacement far too long for the tiny cap"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("core memory full"), std::string::npos);
  EXPECT_EQ(slurp(file), before);
}

TEST(CoreMemoryTool, RemoveSingleMatchDeletesLine) {
  const std::string file = fresh_file("rm");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "keep me"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "drop me"}});
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "drop"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- keep me\n");
  EXPECT_EQ(j["result"].value("entries", -1), 1);
}

TEST(CoreMemoryTool, RemoveNoMatchAndAmbiguousFailClosed) {
  const std::string file = fresh_file("rmN");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "alpha one"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "alpha two"}});
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "ghost"}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "alpha"}})
                   .value("ok", true));
  EXPECT_EQ(slurp(file), "- alpha one\n- alpha two\n");   // both survive both failures
}

TEST(CoreMemoryTool, HandEditedNonBulletLinesAreEntries) {
  const std::string file = fresh_file("hand");
  { std::ofstream f(file); f << "goal: world domination\n"; }   // user-added, no bullet
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "domination"}, {"text", "goal: be helpful"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- goal: be helpful\n");          // canonicalized to a bullet
}

TEST(CoreMemoryTool, EmptyStringArgsAreAbsent) {
  const std::string file = fresh_file("empty");
  // Empty text on add / empty match on remove: missing-arg errors, not weird matches.
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", ""}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file},
                         {{"action", "remove"}, {"match", ""}, {"text", ""}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", ""}, {"text", "x"}})
                   .value("ok", true));
}

TEST(CoreMemoryTool, UnknownActionFails) {
  auto j = call_tool({CORE_MEMORY_BIN, fresh_file("act")}, {{"action", "append"}, {"text", "x"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("add, replace, remove"), std::string::npos);
}

TEST(CoreMemoryTool, StripsEmbeddedNewlinesFromText) {
  const std::string file = fresh_file("nl");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "real fact\n## Injected heading"}});
  std::ifstream f(file);
  std::string l1, l2;
  std::getline(f, l1);
  std::getline(f, l2);
  EXPECT_NE(l1.find("real fact"), std::string::npos);
  EXPECT_NE(l1.find("Injected"), std::string::npos);   // folded onto the SAME line
  EXPECT_TRUE(l2.empty());                              // no second line -> no injected structure
}

TEST(CoreMemoryTool, GarbageCapArgvFallsBackToDefault) {
  const std::string file = fresh_file("gcap");
  // "banana" and "-5" both -> default 2400; a normal add must succeed, and report cap 2400.
  auto j1 = call_tool({CORE_MEMORY_BIN, file, "banana"}, {{"action", "add"}, {"text", "a fact"}});
  ASSERT_TRUE(j1.value("ok", false));
  EXPECT_EQ(j1["result"].value("cap", 0), 2400);
  auto j2 = call_tool({CORE_MEMORY_BIN, file, "-5"}, {{"action", "add"}, {"text", "another"}});
  ASSERT_TRUE(j2.value("ok", false));
  EXPECT_EQ(j2["result"].value("cap", 0), 2400);
}

TEST(CoreMemoryTool, NonStringArgsAndCallFailClosed) {
  ProcResult r1 = run_subprocess({CORE_MEMORY_BIN, fresh_file("ns")},
                                 R"({"call":"core_memory","args":{"action":"add","text":123}})", 30.0);
  auto j1 = nlohmann::json::parse(r1.out, nullptr, false);
  ASSERT_FALSE(j1.is_discarded());
  EXPECT_FALSE(j1.value("ok", true));
  ProcResult r2 = run_subprocess({CORE_MEMORY_BIN}, R"({"call":42})", 30.0);
  auto j2 = nlohmann::json::parse(r2.out, nullptr, false);
  ASSERT_FALSE(j2.is_discarded());
  EXPECT_FALSE(j2.value("ok", true));
}
```

- [ ] **Step 2: Add to CMake and run — expect FAIL (no such binary).** In `CMakeLists.txt`, directly after the `hades-pin-fact` target block (currently lines 85-86):

```cmake
add_executable(hades-core-memory tools/core_memory_main.cpp)
target_link_libraries(hades-core-memory PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-core-memory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

and directly after the `test_pin_fact_tool` rows (currently lines 101-103):

```cmake
target_sources(hades_tests PRIVATE tests/test_core_memory_tool.cpp)
target_compile_definitions(hades_tests PRIVATE CORE_MEMORY_BIN="$<TARGET_FILE:hades-core-memory>")
add_dependencies(hades_tests hades-core-memory)
```

Run: `nix develop --command cmake --build build` → compile error (`tools/core_memory_main.cpp` missing).

- [ ] **Step 3: Implement.** `include/hades/memory_limit.h`:

```cpp
// include/hades/memory_limit.h — default char cap for the always-on core memory
//
// The core-memory file is folded into EVERY turn's system prompt, so every char is paid on
// every LLM call; the cap is the forcing function that makes the agent consolidate instead of
// hoarding (Hermes-agent precedent: 2200). Header-only so the standalone core_memory tool
// binary shares the exact default without linking hades_core (file_version.h precedent).
// Overridden per-manifest via Session { memory_char_limit }.
#pragma once
namespace hades {
inline constexpr long long kDefaultMemoryCharLimit = 2400;
}  // namespace hades
```

`tools/core_memory_main.cpp`:

```cpp
// tools/core_memory_main.cpp — bundled core_memory native tool binary
//
// Bounded, editable core memory: reads one JSON line
// ({"call":"describe"|"core_memory","args":{action,text,match}}) and edits the always-on
// core-memory file. argv (fixed by wiring, never LLM-chosen): [1] file (fallback
// "memory/facts.md"), [2] char cap (fallback/garbage/<=0 -> kDefaultMemoryCharLimit).
// Every non-empty line is an entry; add/replace write canonical "- <text>" bullets (newlines
// fold to spaces — one entry, one line). An add/replace that would push the file over the cap
// fails with a NUMBERED entry list so the model consolidates IN THE SAME TURN (the Hermes
// forcing function). Writes are atomic (temp+rename). Fail-closed: malformed/adversarial
// input returns ok:false, never throws, never partial-writes. Empty-string args = absent.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/memory_limit.h"   // kDefaultMemoryCharLimit (header-only; no core link)

using nlohmann::json;
namespace fs = std::filesystem;

static std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream f(path);
  std::string l;
  while (std::getline(f, l)) lines.push_back(l);
  return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) { out += l; out += '\n'; }
  return out;
}

// Numbered non-empty entries for the consolidation error.
static std::string list_entries(const std::vector<std::string>& lines) {
  std::string out;
  int n = 0;
  for (const auto& l : lines)
    if (!l.empty()) out += std::to_string(++n) + ". " + l + "\n";
  return out;
}

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : "memory/facts.md";
  long long cap = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : hades::kDefaultMemoryCharLimit;
  if (cap <= 0) cap = hades::kDefaultMemoryCharLimit;   // garbage/misconfig must not brick memory

  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);

  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    json out = {{"ok", true},
                {"result",
                 {{"name", "core_memory"},
                  {"description",
                   "Edit your always-on core memory (in your context every turn). action=add "
                   "with text: pin a new standing fact (identity/preferences/facts you always "
                   "need — use save_memory instead for details to recall later). action=replace "
                   "with match+text: rewrite ONE existing entry (match = substring identifying "
                   "it uniquely). action=remove with match: delete ONE entry. Memory is capped; "
                   "when full the error lists every entry — consolidate (merge with replace, "
                   "drop with remove), then retry."},
                  {"schema",
                   {{"type", "object"},
                    {"properties",
                     {{"action", {{"type", "string"}, {"enum", {"add", "replace", "remove"}}}},
                      {"text", {{"type", "string"}}},
                      {"match", {{"type", "string"}}}}},
                    {"required", json::array({"action"})}}}}}};
    std::cout << out.dump() << std::endl;
    return 0;
  }
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (call != "core_memory") return fail("unknown call: " + call);

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  auto str = [&](const char* k) {
    return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string{};
  };
  const std::string action = str("action");
  std::string text = str("text");
  const std::string match = str("match");
  for (char& c : text)
    if (c == '\n' || c == '\r') c = ' ';   // one entry = one line; no injected structure

  // Empty string = absent (the exactly-one-of lesson): validate per action.
  if (action != "add" && action != "replace" && action != "remove")
    return fail("action must be one of: add, replace, remove");
  if ((action == "add" || action == "replace") && text.empty()) return fail("missing arg: text");
  if ((action == "replace" || action == "remove") && match.empty()) return fail("missing arg: match");

  std::vector<std::string> lines = read_lines(file);
  const std::string entry = "- " + text;

  if (action == "add") {
    for (const auto& l : lines)
      if (l == entry) return fail("already pinned: " + entry);
    lines.push_back(entry);
  } else {
    // Substring match over non-empty lines; exactly one hit or fail-closed (never guess).
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < lines.size(); ++i)
      if (!lines[i].empty() && lines[i].find(match) != std::string::npos) hits.push_back(i);
    if (hits.empty()) return fail("no entry matches: " + match);
    if (hits.size() > 1) {
      std::string e = "match is ambiguous (" + std::to_string(hits.size()) + " entries):\n";
      for (auto i : hits) e += lines[i] + "\n";
      return fail(e + "give a longer match that identifies exactly one");
    }
    if (action == "replace") lines[hits[0]] = entry;
    else lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(hits[0]));
  }

  const std::string content = join_lines(lines);
  if ((action == "add" || action == "replace") &&
      static_cast<long long>(content.size()) > cap) {
    // The forcing function: refuse, and hand back everything needed to consolidate NOW.
    return fail("core memory full: this write would make it " + std::to_string(content.size()) +
                "/" + std::to_string(cap) + " chars. Entries:\n" + list_entries(read_lines(file)) +
                "Consolidate: merge or drop entries with replace/remove, then retry.");
  }

  fs::path p(file);
  if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
  const std::string tmp = file + ".tmp";
  std::ofstream f(tmp, std::ios::trunc);
  if (f) { f << content; f.close(); }
  if (!f) { std::remove(tmp.c_str()); return fail("cannot write: " + file); }
  std::error_code ec;
  fs::rename(tmp, file, ec);   // atomic on POSIX; replaces existing
  if (ec) { std::remove(tmp.c_str()); return fail("cannot save: " + file); }

  int entries = 0;
  for (const auto& l : lines) if (!l.empty()) ++entries;
  json result{{"action", action}, {"entries", entries},
              {"chars", static_cast<long long>(content.size())}, {"cap", cap}};
  std::cout << json{{"ok", true}, {"result", result}}.dump() << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R CoreMemoryTool` → all pass. Then the FULL suite (pin_fact tests still present and green).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/memory_limit.h tools/core_memory_main.cpp tests/test_core_memory_tool.cpp CMakeLists.txt
git commit -m "feat: core_memory native tool — bounded editable core memory (add/replace/remove, cap with consolidation error)"
```

---

## Task 2: Wiring + capability + retire pin_fact

**Files:**
- Modify: `app/agent_wiring.cpp` (~lines 180-268: comment, MalConfig check, cap parse, argv append; add include), `src/behaviors/capability_policy.cpp:172`, `tests/test_capability_policy.cpp:76-96`, `CMakeLists.txt` (remove pin-fact rows, add wiring-test rows), `package.nix:12`
- Create: `tests/test_core_memory_wiring.cpp`
- Delete: `tools/pin_fact_main.cpp`, `tests/test_pin_fact_tool.cpp`, `tests/test_pin_fact_wiring.cpp`

**Interfaces:**
- Consumes: `hades-core-memory` binary + `CORE_MEMORY_BIN` compile def (Task 1); `kDefaultMemoryCharLimit` from `hades/memory_limit.h`; existing wiring locals `core_path` (line 192), `parse_ll` (line 205), `tools_resolved` loop (line 264).
- Produces: wiring appends `" <core_path> <memory_char_limit>"` to the `core_memory` tool's `native`; Session key `memory_char_limit` (default 2400, garbage/`<=0` → default); `MalConfig` when `core_memory` is rostered without `memory_file`; `capability_of("core_memory") == Capability::MemoryAppend`. Task 3 relies on the manifest key name `memory_char_limit` and tool name `core_memory`.

- [ ] **Step 1: Write the failing wiring tests** `tests/test_core_memory_wiring.cpp`:

```cpp
// tests/test_core_memory_wiring.cpp — build_agent wires core_memory + live core memory: adding a
// fact writes the core file, the NEXT turn's system prompt contains it, and the Session
// memory_char_limit reaches the tool argv (an over-cap add is refused end-to-end).
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Turn 1: call core_memory add. Turn 2+: plain answer. Captures the messages it was sent.
struct AddThenAnswer : Provider {
  int n = 0;
  std::string add_text = "lives in Patras";
  std::vector<nlohmann::json> seen;
  LlmResponse complete(const LlmRequest& req) override {
    seen.push_back(req.messages.empty() ? nlohmann::json::array() : nlohmann::json(req.messages));
    LlmResponse r;
    if (n++ == 0)
      r.tool_call = {{"id", "c1"}, {"name", "core_memory"},
                     {"arguments", {{"action", "add"}, {"text", add_text}}}};
    else
      r.text = "done";
    return r;
  }
};
}  // namespace

TEST(CoreMemoryWiring, CoreMemoryToolWithoutMemoryFileThrows) {
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  // Session block has NO memory_file -> misconfiguration -> must fail fast.
  EXPECT_THROW(build_agent(bb, std::make_unique<AddThenAnswer>(), tools, {}, "m", Block{}, Block{}),
               MalConfig);
}

TEST(CoreMemoryWiring, AddedFactAppearsInNextTurnSystemPrompt) {
  const std::string core = ::testing::TempDir() + "/wire_cm_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  Block session; session.section = "Session"; session.kv["memory_file"] = core;

  auto prov = std::make_unique<AddThenAnswer>();
  AddThenAnswer* provp = prov.get();
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);

  bb.post("USER_MESSAGE", "remember where I live", "chat"); bb.pump();  // turn 1: model adds
  bb.post("USER_MESSAGE", "where do I live?", "chat");     bb.pump();   // turn 2: model answers

  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(body.find("lives in Patras"), std::string::npos);
  ASSERT_GE(provp->seen.size(), 2u);
  const auto& turn2 = provp->seen.back();
  ASSERT_FALSE(turn2.empty());
  EXPECT_EQ(turn2[0]["role"], "system");
  EXPECT_NE(turn2[0]["content"].get<std::string>().find("lives in Patras"), std::string::npos);
}

TEST(CoreMemoryWiring, MemoryCharLimitReachesToolArgv) {
  const std::string core = ::testing::TempDir() + "/wire_cm_cap_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  Block session; session.section = "Session";
  session.kv["memory_file"] = core;
  session.kv["memory_char_limit"] = "40";        // tiny cap so one long add overflows

  auto prov = std::make_unique<AddThenAnswer>();
  prov->add_text = std::string(100, 'x');        // "- " + 100 chars + "\n" > 40
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);
  nlohmann::json tool_result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { tool_result = e.value; });

  bb.post("USER_MESSAGE", "remember this", "chat"); bb.pump();

  ASSERT_TRUE(tool_result.is_object());
  EXPECT_FALSE(tool_result.value("ok", true));   // over-cap add refused through the real argv
  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_TRUE(body.empty());                      // nothing written
}
```

- [ ] **Step 2: CMake — swap the pin-fact rows.** In `CMakeLists.txt`:
  - DELETE the `hades-pin-fact` target block (lines 85-86: `add_executable(hades-pin-fact tools/pin_fact_main.cpp)` + its `target_link_libraries`).
  - DELETE the `test_pin_fact_tool` rows (lines 101-103: `target_sources … tests/test_pin_fact_tool.cpp`, `PIN_FACT_BIN=…`, `add_dependencies … hades-pin-fact`).
  - Find the `tests/test_pin_fact_wiring.cpp` row (`grep -n test_pin_fact_wiring CMakeLists.txt`) and REPLACE it with:

```cmake
target_sources(hades_tests PRIVATE tests/test_core_memory_wiring.cpp)
```

  (The `CORE_MEMORY_BIN` compile def and `hades-core-memory` dependency already exist from Task 1.)

- [ ] **Step 3: Delete the pin_fact files and run — expect FAIL (wiring still checks `pin_fact`, `core_memory` unwired → MalConfig test red).**

```bash
git rm tools/pin_fact_main.cpp tests/test_pin_fact_tool.cpp tests/test_pin_fact_wiring.cpp
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R CoreMemoryWiring
```

Expected: `CoreMemoryToolWithoutMemoryFileThrows` FAILS (no MalConfig — wiring doesn't know `core_memory` yet); `MemoryCharLimitReachesToolArgv` FAILS (no cap appended).

- [ ] **Step 4: Implement the wiring.** In `app/agent_wiring.cpp`:

1. Add the include next to the other `hades/` includes at the top of the file:

```cpp
#include "hades/memory_limit.h"   // kDefaultMemoryCharLimit
```

2. Update the routing comment (line ~182): `//   pin_fact    -> core file …` becomes:

```cpp
  //   core_memory -> core file (Session `memory_file`),     read live by the Arbiter.
```

3. Replace the `pin_fact` requirement block (lines ~254-260) with:

```cpp
  // core_memory edits the core-memory file the Arbiter folds in each turn; without a
  // configured memory_file the two would target different files (silent drift), so
  // require it explicitly when the core_memory tool is present. The cap (Session
  // memory_char_limit) rides the same argv — single source of truth.
  bool has_core_memory = false;
  for (const auto& t : tools) if (t.name == "core_memory") has_core_memory = true;
  if (has_core_memory && core_path.empty())
    throw MalConfig("core_memory tool requires a memory_file in the Session block");
  long long memory_char_limit = kDefaultMemoryCharLimit;
  if (session.kv.count("memory_char_limit"))
    memory_char_limit = parse_ll(session.kv.at("memory_char_limit"), memory_char_limit);
  if (memory_char_limit <= 0) memory_char_limit = kDefaultMemoryCharLimit;
```

4. Replace the argv branch (lines ~267-268) `else if (t.name == "pin_fact" …)` with:

```cpp
    else if (t.name == "core_memory" && t.kv.count("native") && !core_path.empty())
      t.kv["native"] = t.kv["native"] + " " + core_path + " " + std::to_string(memory_char_limit);
```

5. In `src/behaviors/capability_policy.cpp` line 172, replace:

```cpp
  if (tool == "save_memory" || tool == "pin_fact")       return Capability::MemoryAppend;
```

with:

```cpp
  if (tool == "save_memory" || tool == "core_memory")    return Capability::MemoryAppend;
```

6. In `tests/test_capability_policy.cpp`: line 79 becomes

```cpp
  Action pf{Action::Kind::ToolCall}; pf.tool="core_memory"; pf.args={{"action","add"},{"text","hi"}};
```

and line 95 becomes

```cpp
  EXPECT_EQ(CapabilityPolicy::capability_of("core_memory"), Capability::MemoryAppend);
```

7. In `package.nix` line 12, replace `"hades-pin-fact"` with `"hades-core-memory"` in the `bins` list.

- [ ] **Step 5: Build + full suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green (`-R CoreMemoryWiring` and `-R CapabilityPolicy` first if iterating). Verify no stale references: `grep -rn "pin_fact\|pin-fact" src/ app/ include/ tools/ tests/ CMakeLists.txt package.nix` → expect ZERO hits.
- [ ] **Step 6: Commit.**

```bash
git add -A app/agent_wiring.cpp src/behaviors/capability_policy.cpp tests/test_capability_policy.cpp tests/test_core_memory_wiring.cpp CMakeLists.txt package.nix
git commit -m "feat: wire core_memory (memory_file + memory_char_limit argv, MemoryAppend) and retire pin_fact"
```

(`git rm` from Step 3 is already staged; `git status` must show NO changes to `manifests/` or `memory/facts.md`.)

---

## Task 3: Ship — manifests, soul.md, docs

**Files:**
- Modify: `manifests/dev.hades`, `manifests/pi.hades` (SURGICAL — see Step 1 procedure), `prompts/soul.md`, `docs/manifest-reference.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: tool name `core_memory`, binary `hades-core-memory`, Session key `memory_char_limit` (default 2400) — all fixed by Tasks 1-2.
- Produces: shipped manifests roster `core_memory`; docs describe the new tool + key.

**CRITICAL manifest procedure:** `manifests/dev.hades` and `manifests/pi.hades` carry the USER'S uncommitted live edits (bridge/telegram/voice config) that must NOT be committed. Work on the clean committed version, commit, then re-apply the user's edits:

- [ ] **Step 1: Manifest swap with backup/restore.**

```bash
cp manifests/dev.hades /tmp/dev.hades.user && cp manifests/pi.hades /tmp/pi.hades.user
git checkout -- manifests/dev.hades manifests/pi.hades
# Edit the COMMITTED versions (and the same edit will be re-applied to the user copies):
#   dev.hades: the line `Tool = pin_fact    { native = ./build/hades-pin-fact }` becomes
#              `Tool = core_memory { native = ./build/hades-core-memory }`
#   dev.hades: the memory_file comment `(pin_fact writes it)` becomes `(core_memory edits it)`
#   pi.hades:  the line `Tool = pin_fact    { native = ./bin/hades-pin-fact }` becomes
#              `Tool = core_memory { native = ./bin/hades-core-memory }`
sed -i 's|Tool = pin_fact    { native = ./build/hades-pin-fact }|Tool = core_memory { native = ./build/hades-core-memory }|' manifests/dev.hades
sed -i 's|(pin_fact writes it)|(core_memory edits it)|' manifests/dev.hades
sed -i 's|Tool = pin_fact    { native = ./bin/hades-pin-fact }|Tool = core_memory { native = ./bin/hades-core-memory }|' manifests/pi.hades
git add manifests/dev.hades manifests/pi.hades
# Re-apply the SAME swaps to the user's live copies, then restore them (unstaged):
sed -i 's|Tool = pin_fact    { native = ./build/hades-pin-fact }|Tool = core_memory { native = ./build/hades-core-memory }|' /tmp/dev.hades.user
sed -i 's|(pin_fact writes it)|(core_memory edits it)|' /tmp/dev.hades.user
sed -i 's|Tool = pin_fact    { native = ./bin/hades-pin-fact }|Tool = core_memory { native = ./bin/hades-core-memory }|' /tmp/pi.hades.user
cp /tmp/dev.hades.user manifests/dev.hades && cp /tmp/pi.hades.user manifests/pi.hades
```

After this: `git diff --cached manifests/` shows ONLY the pin_fact→core_memory swaps; `git diff manifests/` shows only the user's pre-existing live edits. Verify both before committing. If a `sed` pattern does not match (whitespace drift), fix the line by hand with the same intent — do NOT skip the file.

- [ ] **Step 2: soul.md memory section.** In `prompts/soul.md`:
  1. Line 21-23 tool roster: replace `` `save_memory`, and `pin_fact` `` with `` `save_memory`, and `core_memory` ``; replace `` `save_memory`\nand `pin_fact` are not `` with `` `save_memory`\nand `core_memory` are not `` (keep the wrapped-line layout intact).
  2. Replace the core-memory bullet (lines 26-28) with:

```markdown
- **Core memory** (`core_memory`): a standing-facts file (`memory/facts.md`) that is **always in your
  context, every turn**. Use action `add` to pin identity, preferences, and facts you always need;
  `replace` / `remove` to keep it current — revise a fact when it changes, drop it when it stops
  being true. The file is capped: when a write is refused as full, the error lists every entry —
  consolidate (merge related entries, drop stale ones), then retry. Your edits appear in this
  prompt immediately (the file is re-read each turn).
```

  3. Line 39: `Both write to your own files (append-only, no confirmation needed).` becomes `Both write to your own files (no confirmation needed).`
  4. Learn-triggers line (line ~54): `` `pin_fact` a preference `` becomes `` `core_memory`-add a preference ``.
  5. Verify: `grep -n pin_fact prompts/soul.md` → zero hits.

- [ ] **Step 3: manifest-reference.md.** Run `grep -n "pin_fact\|pin-fact" docs/manifest-reference.md` and update every hit (currently 3):
  1. Session-table `memory_file` row (line ~113): `` `pin_fact` writes it `` → `` `core_memory` edits it ``; `**Required if the `pin_fact` tool is rostered**` → `**Required if the `core_memory` tool is rostered**`.
  2. Add a new Session-table row directly under `memory_file`:

```markdown
| `memory_char_limit` | Char cap on the core-memory file (it is in EVERY turn's prompt). An over-cap `core_memory` write fails with the entry list so the agent consolidates. | `2400` | Bad/`<=0` value → default. |
```

  3. Argv-append table row (line ~153): `| `pin_fact` | core memory file | `Session.memory_file` | …` becomes:

```markdown
| `core_memory` | core memory file + char cap | `Session.memory_file`, `Session.memory_char_limit` | **requires `Session.memory_file`** (else `MalConfig`) |
```

  4. Capability-table row (line ~247): `| `save_memory`, `pin_fact` | MemoryAppend | **always allow** (append-only to the agent's own files). |` becomes:

```markdown
| `save_memory`, `core_memory` | MemoryAppend | **always allow** (the agent's own memory files; `core_memory` also edits/removes — curation must be frictionless). |
```

  5. If the doc's worked examples or tool listings mention `pin_fact` elsewhere (the grep will show), apply the same rename with `action`-aware wording.

- [ ] **Step 4: CLAUDE.md.** Update (keep terse, house style):
  1. In `## Current state`: the tool roster string `save_memory pin_fact use_skill` → `save_memory core_memory use_skill`.
  2. In the two-memory-layers section: the Core bullet — `pin_fact` tool → `core_memory` tool (`add`/`replace`/`remove`, cap `memory_char_limit` default 2400, over-cap error lists entries for same-turn consolidation; append-only → line-edited; atomic writes).
  3. Targets line: `hades-{…,save-memory,pin-fact,…}` → `…,save-memory,core-memory,…`.
  4. Gotchas: `save_memory`/`pin_fact` store-path whitespace rule → mention both `save_memory`/`core_memory`; the `pin_fact` requires `memory_file` gotcha → `core_memory` requires `memory_file`.
  5. Add a short `### Core memory v2 (shipped 2026-07-11, feat/core-memory)` subsection under Current state: what changed (pin_fact→core_memory, cap+consolidation forcing function, Hermes-inspired), the exactly-one-of empty-arg rule honored, spec/plan paths, and update the test count to the real post-suite number.
- [ ] **Step 5: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green. Also verify the shipped manifest still parses: run the dev.hades lock tests (`-R Manifest`) if present, and `grep -rn "pin_fact" docs/ prompts/ CLAUDE.md` → only historical mentions inside CLAUDE.md's older shipped-feature sections are acceptable (they describe history); zero hits in soul.md and manifest-reference.md.
- [ ] **Step 6: Commit.**

```bash
git add manifests/dev.hades manifests/pi.hades prompts/soul.md docs/manifest-reference.md CLAUDE.md
git commit -m "feat: ship core_memory — manifests, soul.md curation guidance, manifest-reference, CLAUDE.md"
git status   # must show manifests/ modified (user's live edits restored, unstaged) and memory/facts.md untouched
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 558 baseline − 8 pin_fact tests + ~19 new, all green (expected ≈569; record the real number).
2. TSan config if present (`build-tsan/`): rebuild + run — no new threads touched, expect clean.
3. Manual live smoke (Vaios, needs `HADES_API_KEY`):
   ```bash
   nix develop --command ./build/hades manifests/dev.hades
   # user> remember: I prefer answers in Greek
   #   -> agent calls core_memory add; memory/facts.md gains the bullet
   # user> actually make that: prefer answers in Greek, English for code
   #   -> agent calls core_memory replace (match on the old line)
   # user> forget the language preference
   #   -> core_memory remove
   # Fill the file near 2400 chars -> next add returns the consolidation error -> agent merges.
   ```
4. `git status` end state: only the user's live manifest edits + `memory/facts.md` churn remain unstaged.

## Execution

Subagent-driven development (per project process): fresh implementer per task, per-task cpp-reviewer, fix loop for Critical/Important, final whole-branch review, then finishing-a-development-branch (ff-merge via `git branch -f main HEAD`, never push — no remote).
