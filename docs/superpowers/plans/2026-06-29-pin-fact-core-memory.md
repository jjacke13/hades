# pin_fact + live core-memory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add a `pin_fact` tool that appends to an always-on **core memory** file, and make the Arbiter re-read that file every turn so the agent's pins are live the same session. Complements the existing searchable `save_memory` archival store.

**Architecture:** `pin_fact` (native subprocess, append-only) writes `- <text>` to `memory/facts.md` (creating the dir). The Arbiter re-reads that file each turn and folds it into the leading `{role:system}` message (alongside the static SOUL+USER prompt). `assemble_system_prompt` is split: SOUL+USER stay load-once; the MEMORY file becomes a live per-turn read via a new `read_memory_layer()`.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell (`nix develop`) · nlohmann/json · `<filesystem>` · GoogleTest.

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`**.
- After editing `CMakeLists.txt` (new executable / compile def) **reconfigure**: `nix develop --command cmake -S . -B build -G Ninja` then build.
- **Immutability:** never mutate a caller's object in place; copy-then-modify.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Never throw on adversarial input:** tools type-guard (`is_string()`), `read_memory_layer` returns `""` on missing/unset, never throws.
- **`pin_fact` is append-only to the agent's own core file → NOT confirm-gated** (do NOT add it to `AvoidDestructive`).
- **Two layers, distinct:** core = `memory/facts.md`, whole file, every turn, in the system message; archival = `.hades/memory.jsonl`, keyword top-N, ephemeral block before the user msg. Don't merge them.

---

## File Structure

```
tools/pin_fact_main.cpp                T1  binary hades-pin-fact (append "- text" to core file)
include/hades/prompt.h                 T2  (modify) + read_memory_layer()
src/config/prompt.cpp                  T2  (modify) assemble = SOUL+USER; add read_memory_layer
include/hades/arbiter.h                T3  (modify) + memory_path_ + set_memory_path()
src/arbiter/arbiter.cpp                T3  (modify) start_turn folds live core memory
app/agent_wiring.cpp                   T4  (modify) wire pin_fact path + set_memory_path
manifests/dev.hades                    T4  (modify) enable memory_file + pin_fact tool
memory/facts.md                        T4  (new) seed core-memory file
prompts/soul.md                        T4  (modify) describe both memory tools
tests/test_pin_fact_tool.cpp           T1
tests/test_prompt.cpp                  T2  (extend/adjust)
tests/test_arbiter.cpp                 T3  (extend)
tests/test_pin_fact_wiring.cpp         T4
```

---

## Task 1: `pin_fact` native tool

**Files:** Create `tools/pin_fact_main.cpp`, `tests/test_pin_fact_tool.cpp`. Modify `CMakeLists.txt`.

**Interfaces — Produces:** binary `hades-pin-fact`. `{"call":"describe"}` → spec name `pin_fact`. `{"call":"pin_fact","args":{"text":"..."}}` → append `- <text>\n` to `argv[1]` (default `memory/facts.md`), creating the parent dir; return `{"ok":true,"result":{"pinned":true}}`. Absent/non-string text → `{"ok":false,...}`.

- [ ] **Step 1: Write the failing test** — `tests/test_pin_fact_tool.cpp`:

```cpp
// tests/test_pin_fact_tool.cpp — drive the hades-pin-fact binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

TEST(PinFactTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({PIN_FACT_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "pin_fact");
  EXPECT_TRUE(j["result"]["schema"].value("required", nlohmann::json::array()).contains("text"));
}

TEST(PinFactTool, AppendsBulletLineAndCreatesDir) {
  const std::string dir = ::testing::TempDir() + "/pf_dir_" + std::to_string(::getpid());
  std::filesystem::remove_all(dir);
  const std::string file = dir + "/facts.md";   // parent dir does not exist yet
  nlohmann::json call{{"call", "pin_fact"}, {"args", {{"text", "user is based in Greece"}}}};
  ProcResult r = run_subprocess({PIN_FACT_BIN, file}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  std::ifstream f(file);
  std::string line;
  std::getline(f, line);
  EXPECT_EQ(line, "- user is based in Greece");
}

TEST(PinFactTool, AppendDoesNotTruncate) {
  const std::string file = ::testing::TempDir() + "/pf_append.md";
  std::remove(file.c_str());
  auto pin = [&](const std::string& t) {
    nlohmann::json c{{"call", "pin_fact"}, {"args", {{"text", t}}}};
    run_subprocess({PIN_FACT_BIN, file}, c.dump(), 30.0);
  };
  pin("first");
  pin("second");
  std::ifstream f(file);
  std::string l1, l2, l3;
  std::getline(f, l1);
  std::getline(f, l2);
  std::getline(f, l3);
  EXPECT_EQ(l1, "- first");
  EXPECT_EQ(l2, "- second");
  EXPECT_TRUE(l3.empty());
}

TEST(PinFactTool, NonStringTextIsNotOkAndDoesNotCrash) {
  ProcResult r = run_subprocess({PIN_FACT_BIN, ::testing::TempDir() + "/pf_ns.md"},
                                R"({"call":"pin_fact","args":{"text":123}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());        // produced clean JSON, did not abort
  EXPECT_FALSE(j.value("ok", true));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `PIN_FACT_BIN` undefined / target `hades-pin-fact` missing.

- [ ] **Step 3: Write minimal implementation** — `tools/pin_fact_main.cpp`:

```cpp
// tools/pin_fact_main.cpp — bundled pin_fact native tool binary
//
// Reads one JSON line ({"call":"describe"|"pin_fact","args":{text}}), APPENDS one
// markdown bullet "- <text>" to the always-on core-memory file, and writes one JSON
// line. File path = argv[1] (fallback "memory/facts.md"); the parent dir is created if
// missing. Append-only (never truncate) to the agent's own curated file; NOT confirm-
// gated. Type-guarded: a malformed/adversarial request returns ok:false, never throws.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : "memory/facts.md";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "pin_fact"},
             {"description",
              "Pin a standing fact to your always-on core memory — kept in your context "
              "every turn. Use for identity/preferences/standing facts you always need; "
              "use save_memory instead for details to recall by keyword later."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"text", {{"type", "string"}}}}},
               {"required", {"text"}}}}}}};
  } else if (call == "pin_fact") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    bool has_text = args.contains("text") && args["text"].is_string();
    if (!has_text) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      std::string text = args["text"].get<std::string>();
      std::filesystem::path p(file);
      if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);  // best-effort; ofstream reports real failure
      }
      std::ofstream f(file, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + file}}}};
      } else {
        f << "- " << text << "\n";
        out = {{"ok", true}, {"result", {{"pinned", true}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
```

`CMakeLists.txt` — add the executable after the `hades-save-memory` block:
```cmake
add_executable(hades-pin-fact tools/pin_fact_main.cpp)
target_link_libraries(hades-pin-fact PRIVATE nlohmann_json::nlohmann_json)
```
And register the test + binary path (place near the save_memory test wiring):
```cmake
target_sources(hades_tests PRIVATE tests/test_pin_fact_tool.cpp)
target_compile_definitions(hades_tests PRIVATE PIN_FACT_BIN="$<TARGET_FILE:hades-pin-fact>")
add_dependencies(hades_tests hades-pin-fact)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R PinFactTool`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/pin_fact_main.cpp tests/test_pin_fact_tool.cpp CMakeLists.txt
git commit -m "feat: pin_fact native tool (append to always-on core memory file)"
```

---

## Task 2: Split prompt assembly + `read_memory_layer`

**Files:** Modify `include/hades/prompt.h`, `src/config/prompt.cpp`, `tests/test_prompt.cpp`.

**Interfaces:**
- `assemble_system_prompt(const Block&)` — now reads only `system_prompt_file` + `user_file` (drops `memory_file`). Unchanged signature.
- Produces: `std::string read_memory_layer(const std::string& path);` — tolerant whole-file read: empty path → `""`; missing file → `""`; never throws.

- [ ] **Step 1: Write the failing tests** — first READ the current `tests/test_prompt.cpp`. If any existing test asserts that `assemble_system_prompt` includes `memory_file` content, change that test to stop expecting memory_file in the assembled prompt (memory_file is no longer assembled — it is read live elsewhere). Then append these new tests:

```cpp
TEST(Prompt, AssembleJoinsSoulAndUserOnly) {
  const std::string soul = ::testing::TempDir() + "/soul_p.md";
  const std::string user = ::testing::TempDir() + "/user_p.md";
  const std::string mem  = ::testing::TempDir() + "/mem_p.md";
  { std::ofstream(soul) << "SOULTEXT"; }
  { std::ofstream(user) << "USERTEXT"; }
  { std::ofstream(mem)  << "MEMTEXT"; }
  Block s; s.kv["system_prompt_file"] = soul; s.kv["user_file"] = user; s.kv["memory_file"] = mem;
  std::string out = assemble_system_prompt(s);
  EXPECT_NE(out.find("SOULTEXT"), std::string::npos);
  EXPECT_NE(out.find("USERTEXT"), std::string::npos);
  EXPECT_EQ(out.find("MEMTEXT"), std::string::npos);   // memory_file is NOT assembled (read live instead)
}

TEST(Prompt, ReadMemoryLayerTolerant) {
  EXPECT_EQ(read_memory_layer(""), "");                                   // unset
  EXPECT_EQ(read_memory_layer(::testing::TempDir() + "/nope_core.md"), "");// missing file
  const std::string f = ::testing::TempDir() + "/core_present.md";
  { std::ofstream(f) << "- a fact\n"; }
  EXPECT_NE(read_memory_layer(f).find("a fact"), std::string::npos);      // present
}
```
(Ensure `#include <fstream>` and `#include "hades/prompt.h"` are present in the test file.)

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `read_memory_layer` undeclared (and the adjusted assemble test fails if the impl still pulls memory_file).

- [ ] **Step 3: Write minimal implementation**

`include/hades/prompt.h` — update doc + add the declaration:
```cpp
// include/hades/prompt.h — assemble the layered system prompt + read the live core-memory layer
//
// assemble_system_prompt reads system_prompt_file (SOUL) -> user_file (USER) in order from a
// Session Block, joining non-empty contents with a blank line. Throws MalConfig if a configured
// file cannot be read; "" if neither key is set. Loaded once at startup (Arbiter::set_system_prompt).
//
// read_memory_layer reads the always-on MEMORY file (memory_file) fresh — the Arbiter calls it
// every turn so the agent's pin_fact edits are live. Tolerant: ""/missing path -> "", never throws.
#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
std::string assemble_system_prompt(const Block& session);
std::string read_memory_layer(const std::string& path);
}  // namespace hades
```

`src/config/prompt.cpp` — drop `memory_file` from the assembled keys and add the live reader:
```cpp
// src/config/prompt.cpp — assemble_system_prompt (SOUL+USER) + read_memory_layer (live core memory)
//
// assemble_system_prompt iterates the static persona keys (system_prompt_file, user_file), reads each
// configured file (MalConfig on an unreadable path — fail visibly), joins non-empty parts with a blank
// line. The MEMORY layer is NOT assembled here; it is read live per-turn via read_memory_layer.
#include "hades/prompt.h"
#include <array>
#include <fstream>
#include <sstream>
#include "hades/launcher.h"  // MalConfig
namespace hades {
namespace {
std::string read_or_throw(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw MalConfig("system prompt file not readable: " + path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace

std::string assemble_system_prompt(const Block& session) {
  static constexpr std::array<const char*, 2> kKeys = {"system_prompt_file", "user_file"};
  std::string out;
  for (const char* key : kKeys) {
    auto it = session.kv.find(key);
    if (it == session.kv.end() || it->second.empty()) continue;
    std::string content = read_or_throw(it->second);
    if (content.empty()) continue;
    if (!out.empty()) out += "\n\n";
    out += content;
  }
  return out;
}

std::string read_memory_layer(const std::string& path) {
  if (path.empty()) return "";
  std::ifstream f(path);
  if (!f) return "";  // core file may not exist until the first pin_fact — not an error
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace hades
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Prompt`
Expected: PASS (existing prompt tests, adjusted, + 2 new).

- [ ] **Step 5: Commit**

```bash
git add include/hades/prompt.h src/config/prompt.cpp tests/test_prompt.cpp
git commit -m "refactor: assemble_system_prompt = SOUL+USER; add live read_memory_layer"
```

---

## Task 3: Arbiter folds live core memory into the system message

**Files:** Modify `include/hades/arbiter.h`, `src/arbiter/arbiter.cpp`, `tests/test_arbiter.cpp`.

**Interfaces:**
- Consumes: `read_memory_layer` (T2).
- Produces: `Arbiter::set_memory_path(std::string)`; each turn `start_turn()` reads that file fresh and folds it into the leading `{role:system}` message after `system_prompt_`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (add `#include <fstream>` near the top if absent):

```cpp
TEST(Arbiter, FoldsLiveCoreMemoryIntoSystemMessage) {
  const std::string f = ::testing::TempDir() + "/core_arb.md";
  { std::ofstream(f) << "- user prefers metric units\n"; }
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_system_prompt("you are hades");
  a.set_memory_path(f);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  ASSERT_FALSE(req.is_null());
  const auto& sys = req["messages"][0];
  EXPECT_EQ(sys["role"], "system");
  EXPECT_NE(sys["content"].get<std::string>().find("you are hades"), std::string::npos);
  EXPECT_NE(sys["content"].get<std::string>().find("metric units"), std::string::npos);
}

TEST(Arbiter, CoreMemoryIsLiveReloadedEachTurn) {
  const std::string f = ::testing::TempDir() + "/core_live.md";
  { std::ofstream(f) << "- fact one\n"; }
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_memory_path(f);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("USER_MESSAGE","first","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","ok"}}, "llm"); bb.pump();   // turn 1 ends
  { std::ofstream(f, std::ios::app) << "- fact two\n"; }        // agent pins something mid-session
  bb.post("USER_MESSAGE","second","chat"); bb.pump();           // turn 2
  std::string dump = reqs.back()["messages"][0]["content"].get<std::string>();
  EXPECT_NE(dump.find("fact one"), std::string::npos);
  EXPECT_NE(dump.find("fact two"), std::string::npos);          // new pin visible same session
}

TEST(Arbiter, NoCoreMemoryWhenPathUnsetAndNoPrompt) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["role"], "user");   // no leading system message at all
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake --build build`
Expected: FAIL — `set_memory_path` undeclared.

- [ ] **Step 3: Write minimal implementation**

`include/hades/arbiter.h` — add the setter (after `set_system_prompt`) and the member (after `system_prompt_`):
```cpp
  // Path to the always-on core-memory file (memory_file). Re-read every turn so pins are live.
  void set_memory_path(std::string p) { memory_path_ = std::move(p); }
```
```cpp
  std::string memory_path_;     // live core-memory file; re-read each turn into the system message
```

`src/arbiter/arbiter.cpp` — add the include and fold core memory into the system message. Add near the existing includes:
```cpp
#include "hades/prompt.h"   // read_memory_layer
#include <string>
```
Replace the leading-system-message block in `start_turn()` (currently:)
```cpp
  nlohmann::json messages = nlohmann::json::array();
  if (!system_prompt_.empty())
    messages.push_back({{"role", "system"}, {"content", system_prompt_}});
  for (const auto& m : history_) messages.push_back(m);
```
with:
```cpp
  // Leading system message = static SOUL/USER prompt + the live core-memory layer, re-read
  // from disk every turn (so the agent's pin_fact edits show up the same session). Built fresh
  // each turn; never stored in history_.
  nlohmann::json messages = nlohmann::json::array();
  std::string sys = system_prompt_;
  if (!memory_path_.empty()) {
    std::string core = read_memory_layer(memory_path_);
    if (!core.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += core;
    }
  }
  if (!sys.empty())
    messages.push_back({{"role", "system"}, {"content", sys}});
  for (const auto& m : history_) messages.push_back(m);
```
(Leave the `RETRIEVED_MEMORY` ephemeral-block code that follows unchanged.)

- [ ] **Step 4: Run test to verify it passes**

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Arbiter`
Expected: PASS (all prior Arbiter tests + 3 new).

- [ ] **Step 5: Commit**

```bash
git add include/hades/arbiter.h src/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter folds live core-memory file into the system message each turn"
```

---

## Task 4: Wire pin_fact + enable core memory + manifest/seed/persona + integration test

**Files:** Modify `app/agent_wiring.cpp`, `manifests/dev.hades`, `prompts/soul.md`. Create `memory/facts.md`, `tests/test_pin_fact_wiring.cpp`. Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `pin_fact` tool (T1), `read_memory_layer` + assemble split (T2), `set_memory_path` (T3).
- Produces: wiring that derives the core path from Session `memory_file`, appends it to the `pin_fact` tool argv, and calls `set_memory_path`.

- [ ] **Step 1: Write the failing test** — `tests/test_pin_fact_wiring.cpp`:

```cpp
// tests/test_pin_fact_wiring.cpp — build_agent wires pin_fact + live core memory: pinning a fact
// writes the core file, and the NEXT turn's system prompt contains it (end-to-end through the graph).
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Turn 1: call pin_fact. Turn 2+: plain answer. Captures the messages it was sent.
struct PinThenAnswer : Provider {
  int n = 0;
  std::vector<nlohmann::json> seen;
  LlmResponse complete(const LlmRequest& req) override {
    seen.push_back(req.messages.empty() ? nlohmann::json::array() : nlohmann::json(req.messages));
    LlmResponse r;
    if (n++ == 0)
      r.tool_call = {{"id", "c1"}, {"name", "pin_fact"}, {"arguments", {{"text", "lives in Patras"}}}};
    else
      r.text = "done";
    return r;
  }
};
}  // namespace

TEST(PinFactWiring, PinnedFactAppearsInNextTurnSystemPrompt) {
  const std::string core = ::testing::TempDir() + "/wire_core_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());

  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "pin_fact"; t.kv["native"] = PIN_FACT_BIN; tools.push_back(t);
  Block session; session.section = "Session"; session.kv["memory_file"] = core;

  auto prov = std::make_unique<PinThenAnswer>();
  PinThenAnswer* provp = prov.get();
  // build_agent_impl is internal; the Manifest-less overload passes Block{} session. We need the
  // session memory_file wired, so use the manifest overload path via a Manifest, OR the test overload
  // if it forwards a session. This test uses the 6-arg test overload + a Memory block is unused here;
  // the wiring reads memory_file from the SESSION block, so we must reach build_agent_impl with it.
  // The test overload forwards Block{} as session -> memory_file would be lost. Therefore this test
  // drives the graph through a Manifest. See note below.
  (void)provp; (void)session; (void)bb; (void)tools;
  SUCCEED();  // replaced in Step 3 once the wiring exposes a session-carrying seam (see implementer note)
}
```

> **Implementer note (read before Step 3):** the existing test `build_agent` overload forwards `Block{}` as the Session block, so it cannot carry `memory_file`. To test core-memory wiring end-to-end you must reach `build_agent_impl` with a Session block that has `memory_file`. **Add a trailing defaulted `const Block& session = Block{}` parameter to the test `build_agent` overload** (mirroring how Task 6 of the memory plan added `const Block& memory`), forward it into `build_agent_impl` as the `session` argument, and then write the real test body below. Replace the placeholder test with this:

```cpp
TEST(PinFactWiring, PinnedFactAppearsInNextTurnSystemPrompt) {
  const std::string core = ::testing::TempDir() + "/wire_core_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "pin_fact"; t.kv["native"] = PIN_FACT_BIN; tools.push_back(t);
  Block session; session.section = "Session"; session.kv["memory_file"] = core;

  auto prov = std::make_unique<PinThenAnswer>();
  PinThenAnswer* provp = prov.get();
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);

  bb.post("USER_MESSAGE", "remember where I live", "chat"); bb.pump();  // turn 1: model calls pin_fact
  bb.post("USER_MESSAGE", "where do I live?", "chat");     bb.pump();   // turn 2: model answers

  // The core file got the pin, and turn 2's system message (messages[0]) contains it.
  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(body.find("lives in Patras"), std::string::npos);
  ASSERT_GE(provp->seen.size(), 2u);
  const auto& turn2 = provp->seen.back();
  ASSERT_FALSE(turn2.empty());
  EXPECT_EQ(turn2[0]["role"], "system");
  EXPECT_NE(turn2[0]["content"].get<std::string>().find("lives in Patras"), std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build`
Expected: FAIL — the 7-arg `build_agent` overload (with trailing `session`) does not exist yet.

- [ ] **Step 3: Write the implementation**

**(a) `app/agent_wiring.h`** — add a trailing defaulted `const Block& session = Block{}` to the TEST `build_agent` overload declaration (after the existing `const Block& memory = Block{}`):
```cpp
Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model,
                  const Block& memory = Block{},
                  const Block& session = Block{});
```

**(b) `app/agent_wiring.cpp`** — in `build_agent_impl`, generalize the single-source path appending to also cover `pin_fact`, add the whitespace guard for the core path, and set the Arbiter's memory path. Replace the existing store-path block (the `const std::string store_path = ...` through the `tools_resolved` loop) with:
```cpp
  // Single source of truth: each memory tool writes the same file its reader uses.
  //   save_memory -> archival store (Memory block `store`), read by MemoryModule.
  //   pin_fact    -> core file (Session `memory_file`),     read live by the Arbiter.
  // Append each configured path to the matching tool's command; copy-then-modify (never
  // touch the caller's blocks). A path is whitespace-split downstream, so reject whitespace.
  auto reject_ws = [](const std::string& p, const char* what) {
    for (char c : p)
      if (std::isspace(static_cast<unsigned char>(c)))
        throw MalConfig(std::string(what) + " path must not contain whitespace: " + p);
  };
  const std::string store_path =
      memory.kv.count("store") ? memory.kv.at("store") : ".hades/memory.jsonl";
  const std::string core_path = session.kv.count("memory_file") ? session.kv.at("memory_file") : "";
  reject_ws(store_path, "memory store");
  if (!core_path.empty()) reject_ws(core_path, "memory_file");
  std::vector<Block> tools_resolved;
  tools_resolved.reserve(tools.size());
  for (Block t : tools) {
    if (t.name == "save_memory" && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + store_path;
    else if (t.name == "pin_fact" && t.kv.count("native") && !core_path.empty())
      t.kv["native"] = t.kv["native"] + " " + core_path;
    tools_resolved.push_back(std::move(t));
  }
```
And immediately after the existing `a.arbiter->set_system_prompt(assemble_system_prompt(session));` line, add:
```cpp
  a.arbiter->set_memory_path(core_path);  // live core memory (memory_file), re-read each turn
```

**(c) The public overloads** — forward `session`:
- Test overload (`build_agent(bb, llm, tools, objectives, model, memory, session)`): pass `session` as the `session` argument to `build_agent_impl` (instead of the current `Block{}`).
- Manifest overload (`build_agent(bb, m)`): it already has `const Block& s = *session;` — pass `s` as the new `session` arg to `build_agent_impl` (it already does). No behavior change there beyond the new param position. Ensure the call passes `s` for the session parameter.

**(d) `manifests/dev.hades`** — enable the core layer and add the tool. Change line 11 from the comment to active, and add the tool after the `save_memory` tool line:
```
  memory_file        = memory/facts.md   # MEMORY — always-on core memory (pin_fact writes it)
```
```
Tool = pin_fact    { native = ./build/hades-pin-fact }
```

**(e) Seed `memory/facts.md`:**
```
# Core memory — standing facts always kept in the agent's context. The agent appends here via pin_fact.
```

**(f) `prompts/soul.md`** — update the memory bullet list in the "How you work" section so it describes BOTH tools/layers. Replace the existing "Your **memory** is dynamic…" paragraph block with:
```
You have two kinds of memory, each with its own tool:
- **Core memory** (`pin_fact`): a standing-facts file (`memory/facts.md`) that is **always in your
  context, every turn**. Call `pin_fact(text)` for identity, preferences, and facts you always need.
  Your pins appear in this prompt immediately (the file is re-read each turn).
- **Archival memory** (`save_memory`): a searchable store (`.hades/memory.jsonl`). Call
  `save_memory(text)` for details to keep for later; each turn the most relevant entries are recalled
  by **keyword** match against the user's message and shown in a "Relevant memories:" block. If nothing
  matches, none appear. (Retrieval is keyword-based for now, not semantic.)
Both write to your own files (append-only, no confirmation needed). Describe this plainly when asked.
```

**(g) `CMakeLists.txt`** — register the wiring test:
```cmake
target_sources(hades_tests PRIVATE tests/test_pin_fact_wiring.cpp)
```
(`PIN_FACT_BIN` + `add_dependencies(hades_tests hades-pin-fact)` from Task 1 already cover this test.)

- [ ] **Step 4: Run the full suite**

Run: `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`
Expected: all green (prior 85 + the new pin_fact/core-memory tests). Existing `test_e2e` / `test_memory_wiring` still pass via the defaulted trailing params.

- [ ] **Step 5: Commit**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp manifests/dev.hades memory/facts.md prompts/soul.md tests/test_pin_fact_wiring.cpp CMakeLists.txt
git commit -m "feat: wire pin_fact + live core memory (memory_file enabled, persona updated)"
```

---

## Self-Review (against the spec)

- **Spec coverage:** pin_fact tool with dir-create + type-guard (T1) · assemble split + read_memory_layer (T2) · Arbiter live core fold (T3) · wiring single-source path + set_memory_path + manifest/seed/persona + integration (T4). save_memory NOT renamed; pin_fact NOT confirm-gated (no AvoidDestructive change). All spec sections mapped.
- **Two channels stay distinct:** core memory → leading system message (T3); archival `RETRIEVED_MEMORY` → ephemeral pre-user block (unchanged).
- **Type consistency:** `read_memory_layer(path)`, `set_memory_path(string)`, `pin_fact`/`memory_file`/`core_path`, trailing defaulted `session` param — consistent across tasks.
- **No placeholders:** the only deferred snippet is the Task-4 test body, explicitly replaced in Step 3 with the real test after the overload seam is added.

## Verification

1. `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → all green.
2. Live smoke (needs key):
```bash
export HADES_API_KEY=...
nix develop --command ./build/hades manifests/dev.hades
# user> pin that I prefer the metric system as a core fact
# user> what are my core facts?      # should answer from the always-on layer, same session
cat memory/facts.md                  # the pinned bullet
```
