# session_search Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A native `session_search` tool — explicit keyword search over past-session jsonls returning raw per-turn excerpts.

**Architecture:** Relocate the pure `extract_session_turns` parser into `src/core/session.cpp` so the standalone tool binary can compile it in (cron_store precedent); the tool scores per-turn units by lowercased token overlap; wiring pins the sessions dir + live-session filename via argv; new `Capability::SessionRead` → always allow.

**Tech Stack:** C++20, CMake+Ninja inside `nix develop`, nlohmann_json, GoogleTest.

Spec: `docs/superpowers/specs/2026-07-16-session-search-design.md` (committed on this branch).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build` then `nix develop --command ctest --test-dir build --output-on-failure`. Baseline **662/662** green before Task 1.
- Branch `feat/session-search` (already created; spec committed `52fca75`). Commit style `<type>: <desc>`, NO attribution footer.
- Tool binaries never link `hades_core` — they compile shared pure sources into themselves and include headers only.
- The hades native tool protocol: one JSON line on stdin (`{"call":"describe"|"<tool>","args":{…}}`), one JSON line on stdout (`{"ok":bool,"result":{…}}`); malformed input must yield `ok:false`, never a throw.
- Empty-string args count as ABSENT (house rule, `833b9aa`): an empty `query` fails closed with a clear error.
- Result unit text truncated to **700** chars; `max_results` default **5**, clamped **1..20**; live session excluded by FILENAME comparison (argv[2]).
- Do NOT touch `manifests/dev.local.hades`, `manifests/pi.hades`, `memory/`, or anything gitignored.

## File Structure

```
src/core/session.cpp                     T1  gains extract_session_turns (verbatim move)
src/apps/embedding_memory/embedding_memory.cpp  T1  loses it (include stays)
tools/session_search_main.cpp            T2  the tool binary
tests/test_session_search_tool.cpp       T2
include/hades/objective/capability_policy.h    T3  SessionRead enum
src/behaviors/capability_policy.cpp      T3  capability_of row + veto allow
tests/test_capability_policy.cpp         T3  (append)
app/agent_wiring.cpp                     T3  argv append (dir + live filename)
tests/test_session_search_wiring.cpp     T3
docs/manifest-reference.md, CLAUDE.md, manifests/dev.hades  T3  ship rows
CMakeLists.txt                           T2, T3
```

---

## Task 1: Relocate `extract_session_turns` into `src/core/session.cpp`

**Files:**
- Modify: `src/apps/embedding_memory/embedding_memory.cpp` (remove the `── session_turns` banner section: the `role_of` static helper + `extract_session_turns` — keep the `#include "hades/embedding/session_turns.h"` and the `vec_math` section that follows)
- Modify: `src/core/session.cpp` (append the same code verbatim)

**Interfaces:**
- Consumes: existing `read_session_jsonl` (already in `src/core/session.cpp`), `include/hades/embedding/session_turns.h` (untouched).
- Produces: `hades::extract_session_turns(const std::string&)` now lives in `src/core/session.cpp` — Task 2's tool compiles that file in.

- [ ] **Step 1: Move the code.** Cut from `embedding_memory.cpp` the block that starts with the banner comment `// ── session_turns: extract per-turn "U:…\nA:…" units from a session jsonl (was src/embedding/session_turns.cpp) ──────────────` and ends just before the `// ── vec_math:` banner (it contains `namespace hades { static std::string role_of(...) ... extract_session_turns(...) ... }`). Paste it VERBATIM (including the banner, updating only the parenthetical to `(moved 2026-07-16 from embedding_memory.cpp so tool binaries can compile session.cpp standalone)`) at the END of `src/core/session.cpp`. Add `#include <nlohmann/json.hpp>` and `#include "hades/embedding/session_turns.h"` to `session.cpp`'s include block if not already present (`read_session_jsonl` already returns `nlohmann::json`, so the json include exists via `session_history.h`; add the session_turns include).
- [ ] **Step 2: Build + full suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → **662/662** (the existing `test_session_turns.cpp` covers the moved code; no test changes).
- [ ] **Step 3: Commit.**

```bash
git add src/core/session.cpp src/apps/embedding_memory/embedding_memory.cpp
git commit -m "refactor: move extract_session_turns to src/core/session.cpp (pure parsing; lets tool binaries compile it in)"
```

---

## Task 2: The `session_search` tool binary

**Files:**
- Create: `tools/session_search_main.cpp`
- Test: `tests/test_session_search_tool.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `extract_session_turns` + `read_session_jsonl` (via compiling `src/core/session.cpp` into the binary), `include/hades/embedding/session_turns.h`.
- Produces: binary `hades-session-search`; call `session_search` args `{query, max_results?}` → `{"hits":[{"session","turn","text"}...],"searched_sessions":N}`; argv: `[1]`=sessions dir (default `.hades/sessions`), `[2]`=live-session filename to exclude (optional).

- [ ] **Step 1: Write the failing tests** `tests/test_session_search_tool.cpp`:

```cpp
// tests/test_session_search_tool.cpp — drive the hades-session-search binary over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
std::string fresh_dir(const char* tag) {
  const std::string d =
      ::testing::TempDir() + "/sess_search_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
void write_session(const std::string& dir, const std::string& name,
                   const std::vector<std::pair<std::string, std::string>>& turns) {
  std::ofstream f(dir + "/" + name);
  for (const auto& [u, a] : turns) {
    f << nlohmann::json{{"role", "user"}, {"content", u}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", a}}.dump() << "\n";
  }
}
nlohmann::json search(const std::vector<std::string>& argv_tail, const nlohmann::json& args) {
  std::vector<std::string> argv{SESSION_SEARCH_BIN};
  for (const auto& s : argv_tail) argv.push_back(s);
  nlohmann::json call{{"call", "session_search"}, {"args", args}};
  ProcResult r = run_subprocess(argv, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(SessionSearchTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SESSION_SEARCH_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "session_search");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "query") != required.end());
}

TEST(SessionSearchTool, RanksByTokenOverlapNewestFirst) {
  const std::string dir = fresh_dir("rank");
  write_session(dir, "20260701-100000.jsonl",
                {{"tell me about the pi zero deployment", "we deployed hades to the pi"}});
  write_session(dir, "20260710-100000.jsonl",
                {{"unrelated chat about weather", "sunny"},
                 {"pi zero deployment status?", "pi deployment is live"}});
  auto j = search({dir}, {{"query", "pi deployment"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  const auto& hits = j["result"]["hits"];
  ASSERT_GE(hits.size(), 2u);
  // Both matching turns found; the newest session's hit ranks first on equal overlap.
  EXPECT_EQ(hits[0].value("session", ""), "20260710-100000");
  EXPECT_NE(hits[0].value("text", "").find("pi deployment is live"), std::string::npos);
  EXPECT_EQ(j["result"].value("searched_sessions", 0), 2);
  // The weather turn (zero overlap) is not a hit.
  for (const auto& h : hits)
    EXPECT_EQ(h.value("text", "").find("sunny"), std::string::npos);
}

TEST(SessionSearchTool, LiveSessionExcludedByFilename) {
  const std::string dir = fresh_dir("live");
  write_session(dir, "old.jsonl", {{"magic keyword alpha", "noted"}});
  write_session(dir, "live.jsonl", {{"magic keyword alpha", "live copy"}});
  auto j = search({dir, "live.jsonl"}, {{"query", "magic keyword alpha"}});
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["hits"].size(), 1u);
  EXPECT_EQ(j["result"]["hits"][0].value("session", ""), "old");
  EXPECT_EQ(j["result"].value("searched_sessions", 0), 1);
}

TEST(SessionSearchTool, MaxResultsClampAndTruncation) {
  const std::string dir = fresh_dir("clamp");
  std::vector<std::pair<std::string, std::string>> turns;
  for (int i = 0; i < 30; ++i)
    turns.push_back({"needle number " + std::to_string(i), std::string(2000, 'x')});
  write_session(dir, "s.jsonl", turns);
  auto j = search({dir}, {{"query", "needle"}, {"max_results", 999}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"]["hits"].size(), 20u);                       // clamped to 20
  EXPECT_LE(j["result"]["hits"][0].value("text", "").size(), 700u); // unit truncated
  auto j2 = search({dir}, {{"query", "needle"}});
  EXPECT_EQ(j2["result"]["hits"].size(), 5u);                       // default 5
}

TEST(SessionSearchTool, EmptyQueryFailsClosed) {
  const std::string dir = fresh_dir("empty");
  for (const char* raw :
       {R"({"call":"session_search","args":{}})",
        R"({"call":"session_search","args":{"query":""}})",
        R"({"call":"session_search","args":{"query":42}})"}) {
    ProcResult r = run_subprocess({SESSION_SEARCH_BIN, dir}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}

TEST(SessionSearchTool, NoHitsAndMissingDirAreOkEmpty) {
  const std::string dir = fresh_dir("nohits");
  write_session(dir, "s.jsonl", {{"hello there", "hi"}});
  auto j = search({dir}, {{"query", "zzz_nomatch_zzz"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"]["hits"].empty());
  auto j2 = search({"/nonexistent/sessions/dir"}, {{"query", "anything"}});
  ASSERT_TRUE(j2.value("ok", false));
  EXPECT_TRUE(j2["result"]["hits"].empty());
  EXPECT_EQ(j2["result"].value("searched_sessions", -1), 0);
}

TEST(SessionSearchTool, CorruptLinesSkippedNonJsonlIgnored) {
  const std::string dir = fresh_dir("corrupt");
  {
    std::ofstream f(dir + "/c.jsonl");
    f << "{not json\n";
    f << nlohmann::json{{"role", "user"}, {"content", "findable token here"}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", "answer"}}.dump() << "\n";
  }
  std::ofstream(dir + "/notes.txt") << "findable token here but wrong extension\n";
  auto j = search({dir}, {{"query", "findable token"}});
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["hits"].size(), 1u);
  EXPECT_EQ(j["result"]["hits"][0].value("session", ""), "c");
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt`, next to the other tool blocks (after the `hades-cancel-task` block), add:

```cmake
add_executable(hades-session-search tools/session_search_main.cpp src/core/session.cpp)
target_link_libraries(hades-session-search PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-session-search PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(hades_tests PRIVATE tests/test_session_search_tool.cpp)
target_compile_definitions(hades_tests PRIVATE SESSION_SEARCH_BIN="$<TARGET_FILE:hades-session-search>")
add_dependencies(hades_tests hades-session-search)
```

Run: `nix develop --command cmake --build build` → compile error (no such file `tools/session_search_main.cpp`).

- [ ] **Step 3: Implement** `tools/session_search_main.cpp`:

```cpp
// tools/session_search_main.cpp — bundled session_search native tool binary
//
// Explicit full-text recall over PAST sessions: reads one JSON line
// ({"call":"describe"|"session_search","args":{query, max_results?}}), splits every
// <sessions_dir>/*.jsonl into per-turn "U:…\nA:…" units (extract_session_turns, compiled in via
// src/core/session.cpp — no core link) and ranks them by lowercased token overlap with the
// query (the rank_memories idiom). argv[1] = sessions dir (wiring-pinned; fallback
// ".hades/sessions"), argv[2] = live-session FILENAME to exclude (the Arbiter already holds
// that context in-history). Complements the auto-injected embedding recall: this is the
// deliberate, exact "did we discuss X?" path. Raw excerpts only — summarizing is the caller's
// job. Fail-closed on malformed input; no hits is ok:true with an empty list, not an error.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/embedding/session_turns.h"   // extract_session_turns (impl: src/core/session.cpp)

namespace {
constexpr std::size_t kDefaultResults = 5;
constexpr std::size_t kMaxResults     = 20;
constexpr std::size_t kUnitTruncate   = 700;

std::set<std::string> tokens_of(const std::string& s) {
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

struct Hit {
  std::size_t score;
  std::string session;  // file stem — timestamp ids sort lexically = chronologically
  std::size_t turn;
  std::string text;
};
}  // namespace

int main(int argc, char** argv) {
  const std::string dir  = argc > 1 ? argv[1] : ".hades/sessions";
  const std::string live = argc > 2 ? argv[2] : "";
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
            {{"name", "session_search"},
             {"description",
              "Search your PAST conversation sessions by keywords and get back the matching "
              "user/assistant exchanges verbatim. Use it to answer \"did we discuss X?\" or to "
              "recover details the automatic memory recall did not surface. The current "
              "conversation is not searched (you already have it)."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"query", {{"type", "string"}}},
                 {"max_results",
                  {{"type", "integer"},
                   {"description", "how many excerpts to return (default 5, max 20)"}}}}},
               {"required", {"query"}}}}}}};
  } else if (call == "session_search") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    const bool has_q = args.contains("query") && args["query"].is_string();
    const std::string query = has_q ? args["query"].get<std::string>() : "";
    const auto qtok = tokens_of(query);
    if (query.empty() || qtok.empty()) {                 // empty = absent (house rule)
      out = {{"ok", false}, {"result", {{"error", "missing arg: query (non-empty keywords)"}}}};
    } else {
      std::size_t max_results = kDefaultResults;
      if (args.contains("max_results") && args["max_results"].is_number_integer()) {
        const long long m = args["max_results"].get<long long>();
        if (m > 0) max_results = std::min<std::size_t>(static_cast<std::size_t>(m), kMaxResults);
      }
      std::vector<Hit> hits;
      int searched = 0;
      std::error_code ec;
      std::filesystem::directory_iterator it(dir, ec), end;
      for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        if (it->path().extension() != ".jsonl") continue;
        if (!live.empty() && it->path().filename().string() == live) continue;  // live session
        ++searched;
        const std::string stem = it->path().stem().string();
        std::size_t idx = 0;
        for (const auto& t : extract_session_turns(it->path().string())) {
          const auto utok = tokens_of(t.text);
          std::size_t score = 0;
          for (const auto& q : qtok)
            if (utok.count(q)) ++score;
          if (score > 0) {
            std::string text = t.text.substr(0, kUnitTruncate);
            hits.push_back({score, stem, idx, std::move(text)});
          }
          ++idx;
        }
      }
      std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;      // best overlap first
        if (a.session != b.session) return a.session > b.session;  // newer session first
        return a.turn > b.turn;                                // later turn first
      });
      if (hits.size() > max_results) hits.resize(max_results);
      nlohmann::json jhits = nlohmann::json::array();
      for (const auto& h : hits)
        jhits.push_back({{"session", h.session}, {"turn", h.turn}, {"text", h.text}});
      out = {{"ok", true},
             {"result", {{"hits", jhits}, {"searched_sessions", searched}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump() << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SessionSearchTool` → all pass; then the full suite → green.
- [ ] **Step 5: Commit.**

```bash
git add tools/session_search_main.cpp tests/test_session_search_tool.cpp CMakeLists.txt
git commit -m "feat: session_search native tool — keyword recall over past-session jsonls"
```

---

## Task 3: Capability, wiring, ship

**Files:**
- Modify: `include/hades/objective/capability_policy.h` (enum), `src/behaviors/capability_policy.cpp` (`capability_of` + `veto`)
- Test: `tests/test_capability_policy.cpp` (append)
- Modify: `app/agent_wiring.cpp` (argv append in the `tools_resolved` loop)
- Test: `tests/test_session_search_wiring.cpp` (create)
- Modify: `CMakeLists.txt`, `docs/manifest-reference.md`, `manifests/dev.hades`, `CLAUDE.md`

**Interfaces:**
- Consumes: the T2 binary + the existing `session_path` parameter of `wire_agent`/`build_agent` (already threaded from `hades_main`) and the `Session.sessions_dir` key (default `.hades/sessions`).
- Produces: `Capability::SessionRead` (→ allow); `session_search` argv = `<native> <sessions_dir> [<live filename>]`.

- [ ] **Step 1: Failing capability tests** — append to `tests/test_capability_policy.cpp`:

```cpp
TEST(CapabilityPolicy, SessionSearchIsSessionReadAndAllowed) {
  EXPECT_EQ(CapabilityPolicy::capability_of("session_search"), Capability::SessionRead);
  CapabilityScope sc;              // confirm_unscoped default true — proves NOT Unknown->confirm
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action a{Action::Kind::ToolCall};
  a.tool = "session_search";
  a.args = {{"query", "pi deployment"}};
  EXPECT_FALSE(p.veto(bb, a).vetoed);
}
```

- [ ] **Step 2: Implement capability.** In `include/hades/objective/capability_policy.h`, add `SessionRead` to the enum right after `MemoryAppend`. In `src/behaviors/capability_policy.cpp`:
  - in `capability_of`, next to the memory rows: `if (tool == "session_search") return Capability::SessionRead;`
  - in the `veto` switch, add `case Capability::SessionRead:` beside the `MemoryAppend` case with the comment: `// The agent's own past-session files: dir fixed by wiring argv, read-only. A peer-driven turn can read excerpts out — the documented Bridge-SECURITY class; per-origin scopes (capability v2) are the real fix.` → `return allow();`
- [ ] **Step 3: Run capability tests** — `-R CapabilityPolicy` → pass.
- [ ] **Step 4: Failing wiring test** `tests/test_session_search_wiring.cpp`:

```cpp
// tests/test_session_search_wiring.cpp — wiring appends sessions dir + live filename to argv
// Roster has NO llm module: the ToolRunner runs the REAL binary; we drive TOOL_REQUEST directly.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;
namespace fs = std::filesystem;

TEST(SessionSearchWiring, ArgvCarriesSessionsDirAndExcludesLiveFile) {
  const std::string dir =
      ::testing::TempDir() + "/ss_wire_" + std::to_string(::getpid());
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto write_session = [&](const std::string& name, const std::string& u, const std::string& a) {
    std::ofstream f(dir + "/" + name);
    f << nlohmann::json{{"role", "user"}, {"content", u}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", a}}.dump() << "\n";
  };
  write_session("past.jsonl", "the zeta needle fact", "stored");
  write_session("live.jsonl", "the zeta needle fact", "live copy");
  const std::string manifest =
      "Session\n{\n  model = m\n  sessions_dir = " + dir + "\n}\n" +
      "Module = tool_runner\nModule = arbiter\n" +
      "Tool = session_search { native = " + std::string(SESSION_SEARCH_BIN) + " }\n";
  Blackboard bb;
  Manifest m = parse_manifest(manifest);
  Agent agent = build_agent(bb, m, dir + "/live.jsonl");   // live session path threaded
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.post("TOOL_REQUEST",
          {{"id", "s1"}, {"tool", "session_search"}, {"args", {{"query", "zeta needle"}}}},
          "arbiter");
  bb.pump();
  ASSERT_TRUE(result.is_object());
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  const auto& hits = result["content"]["hits"];
  ASSERT_EQ(hits.size(), 1u) << result.dump();             // live.jsonl excluded via argv
  EXPECT_EQ(hits[0].value("session", ""), "past");
}
```

CMake (next to the T2 test lines):

```cmake
target_sources(hades_tests PRIVATE tests/test_session_search_wiring.cpp)
```

Note: the ToolRunner posts the tool's `result` object as `content` — if the existing e2e tests (e.g. `tests/test_mcp_discovery.cpp` `ToolRunnerCallsRealNameEndToEnd`) show a different envelope shape, match THAT idiom rather than this sketch.

- [ ] **Step 5: Implement wiring.** In `app/agent_wiring.cpp`:
  1. `wire_agent` already receives `session_path` (may be `""`). Resolve the sessions dir near the other path resolutions (before the `tools_resolved` loop):

```cpp
  // session_search: pin the search root (+ live-session filename to exclude) via argv —
  // the SAME Session.sessions_dir resolution hades_main uses; single source of truth.
  std::string sessions_dir = ".hades/sessions";
  if (s.kv.count("sessions_dir") && !s.kv.at("sessions_dir").empty())
    sessions_dir = s.kv.at("sessions_dir");
  reject_ws(sessions_dir, "sessions dir");
```

  (Place `reject_ws` usage exactly like the `skills_dir` line. `s` is the Session block variable already in scope — check its actual name in the function and use that.)

  2. In the `tools_resolved` loop, after the `list_tasks`/`cancel_task` branch:

```cpp
    else if (t.name == "session_search" && t.kv.count("native")) {
      t.kv["native"] = t.kv["native"] + " " + sessions_dir;
      if (!session_path.empty())
        t.kv["native"] += " " + std::filesystem::path(session_path).filename().string();
    }
```

  (Add `#include <filesystem>` to `agent_wiring.cpp` if absent.)

- [ ] **Step 6: Build + run new tests** — `-R SessionSearch` → all pass; full suite green.
- [ ] **Step 7: Ship docs.**
  - `manifests/dev.hades`: after the `run_command` block add:

```
# Tool = session_search { native = ./build/hades-session-search }   # keyword recall over past sessions
```

  - `docs/manifest-reference.md` §4: argv-append table row: `session_search` → `<sessions_dir> [<live-session filename>]` (from `Session.sessions_dir` + the resolved live session; live file excluded from search). Verdict table row (after `save_memory, core_memory`): `session_search` | SessionRead | **always allow** (own session files, dir wiring-pinned; peer-turn read-out caveat — see Bridge SECURITY).
  - `CLAUDE.md`: one short subsection under Current state (`### session_search (shipped 2026-07-16, feat/session-search)` — tool contract, relocation note, SessionRead, test count) + tick item 2b off the memory-v2 work-list + update the current-state test count.
- [ ] **Step 8: Full suite + commit.**

```bash
git add include/hades/objective/capability_policy.h src/behaviors/capability_policy.cpp \
  tests/test_capability_policy.cpp app/agent_wiring.cpp tests/test_session_search_wiring.cpp \
  CMakeLists.txt docs/manifest-reference.md manifests/dev.hades CLAUDE.md
git commit -m "feat: wire session_search — SessionRead capability, sessions-dir argv, ship docs"
```

---

## Verification (end-to-end)

1. Full suite green inside `nix develop` (baseline 662 + ~10 new).
2. Manual live smoke (Vaios): `Tool = session_search` in dev.local.hades → "did we discuss the pi deployment before?" → tool call returns excerpts from a past session; "search our old sessions for X" honors max_results.
3. Security spot-check: peer-turn caveat documented; argv dir pinned (LLM passing a `dir` arg has no effect — schema has no such property and argv wins).

## Execution

Subagent-driven development: fresh opus implementer per task, opus cpp-reviewer per task, final whole-branch review, then finishing-a-development-branch (ff merge to main; push only on Vaios's word).
