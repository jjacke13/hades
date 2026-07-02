# hades Skills System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Context

hades (C++20 agent harness on the MOOS-IvP architecture: Blackboard pub/sub + Arbiter turn loop + subprocess tools + manifest-driven module roster) gets a **skills system**: loadable instruction packs the agent can discover, invoke, and **author itself**. Spec: `docs/superpowers/specs/2026-07-02-skills-system-design.md` (approved, committed `d8eda7e` on branch `feat/skills`).

A skill = `skills/<name>/SKILL.md` (frontmatter `name:`/`description:` + markdown body) with optional `scripts/` run via the existing `shell` tool. A dedicated `SkillsModule` scans the dir and posts a preformatted `SKILLS_ANNOUNCE` string to the Blackboard (at attach + rescan when a `save_skill` tool call succeeds — **no per-turn scanning**); the Arbiter folds it into the leading system message via `bb_->get()` (no subscription). Two new native subprocess tools: `use_skill` (load a skill's body → TOOL_RESULT → persists in history) and `save_skill` (agent writes/overwrites a skill; atomic temp+rename). Both strictly validate the skill name (`[A-Za-z0-9_-]{1,64}`) — a traversal name is an arbitrary read/write escape. New capabilities `SkillRead`/`SkillWrite` → allow (pin_fact precedent). **Zero coupling:** any combination of {skill tools} × {skills module} is legal; dir defaults to `skills`.

**Goal:** Agent sees its skills roster in the system prompt, loads one with `use_skill`, and distills new ones with `save_skill` — announced live the same session.

**Architecture:** Pure scan lib (`src/skills/scan.cpp`) → SkillsModule (bus announce) → Arbiter fold (get()) → two self-describing tool binaries (wiring appends the resolved skills dir to their argv, pin_fact pattern).

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, std::filesystem.

## Global Constraints

- **Every build/test command runs inside `nix develop`**: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **251/251 tests green** before Task 1.
- Branch `feat/skills` (already created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- Skill name validation is **exactly** `[A-Za-z0-9_-]`, length 1..64, defined **once** in `include/hades/skills/scan.h` as `inline bool valid_skill_name` and included by BOTH tool binaries (header-only — tools do not link hades_core).
- Announce header line is **exactly**: `Available skills (call use_skill with a name to load its full instructions):` followed by `\n- <name>: <description>` per skill, sorted by name, no trailing newline. Empty library → `""`.
- Skills dir default is **exactly** `"skills"`; resolved ONLY via `resolve_skills_dir(const Block&)` (single source of truth for module + tool argv).
- Bus keys: `SKILLS_ANNOUNCE` (string). Existing payloads: `TOOL_REQUEST {id, tool, args}`, `TOOL_RESULT {id, ok, content}`.
- Pump-thread handlers must **never throw** (try/catch pattern; degrade, don't crash).
- File headers: `// path — one-line purpose` + short explanation block (house style).
- `manifests/dev.hades` currently carries the user's uncommitted edits (`model = gpt-5.5`, `reindex_interval_s = 40`). Decision (Vaios): **commit everything** — the Task 8 dev.hades commit includes those local edits as the new defaults. Do NOT revert them. `memory/facts.md` working-tree changes: leave untouched, never stage.

---

## File Structure

```
include/hades/skills/scan.h          T1  pure scan API + inline valid_skill_name
src/skills/scan.cpp                  T1  frontmatter parse, dir scan, announce format, resolve dir
tests/test_skills_scan.cpp           T1
tools/use_skill_main.cpp             T2  hades-use-skill binary
tests/test_use_skill_tool.cpp        T2
tools/save_skill_main.cpp            T3  hades-save-skill binary
tests/test_save_skill_tool.cpp       T3
include/hades/module/skills_module.h T4  SkillsModule
src/module/skills_module.cpp         T4
tests/test_skills_module.cpp         T4
src/arbiter/arbiter.cpp              T5  fold SKILLS_ANNOUNCE into leading system msg (modify)
tests/test_arbiter.cpp               T5  (append tests)
include/hades/objective/capability_policy.h  T6  enum + table (modify)
src/objective/capability_policy.cpp  T6  (modify)
tests/test_capability_policy.cpp     T6  (append tests)
app/agent_wiring.h / .cpp            T7  Agent.skills member, factory, argv append (modify)
tests/test_skills_wiring.cpp         T7
manifests/dev.hades, prompts/soul.md, CLAUDE.md  T8  ship (modify)
CMakeLists.txt                       T1,T2,T3,T4,T7 (add sources/targets)
```

---

## Task 1: Skills scan library (pure)

**Files:**
- Create: `include/hades/skills/scan.h`, `src/skills/scan.cpp`
- Test: `tests/test_skills_scan.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**
- `struct SkillInfo { std::string name; std::string description; };`
- `inline bool valid_skill_name(const std::string& n)` — header-only (tools include it without linking core)
- `std::string parse_skill_description(const std::string& text)` — `""` when unparseable
- `std::vector<SkillInfo> scan_skills_dir(const std::string& dir)` — sorted by name; missing dir → `{}`
- `std::string format_skills_announce(const std::vector<SkillInfo>&)` — `""` when empty
- `std::string resolve_skills_dir(const Block& skills_cfg)` — `dir` key or default `"skills"`

- [ ] **Step 1: Write the failing tests** `tests/test_skills_scan.cpp`:

```cpp
// tests/test_skills_scan.cpp — pure skills scan lib: frontmatter, name gate, scan, announce
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/skills/scan.h"
using namespace hades;
namespace fs = std::filesystem;

static void write_skill(const std::string& root, const std::string& name,
                        const std::string& content) {
  fs::create_directories(root + "/" + name);
  std::ofstream f(root + "/" + name + "/SKILL.md");
  f << content;
}

TEST(SkillsScan, ParsesDescriptionFromFrontmatter) {
  EXPECT_EQ(parse_skill_description("---\nname: x\ndescription: does things\n---\nbody"),
            "does things");
  EXPECT_EQ(parse_skill_description("---\ndescription:   padded   \n---\n"), "padded");
}

TEST(SkillsScan, UnparseableYieldsEmpty) {
  EXPECT_EQ(parse_skill_description(""), "");
  EXPECT_EQ(parse_skill_description("no frontmatter at all"), "");
  EXPECT_EQ(parse_skill_description("---\ndescription: never closed"), "");   // no closing fence
  EXPECT_EQ(parse_skill_description("---\nname: x\n---\nbody"), "");          // no description
}

TEST(SkillsScan, ValidSkillNameGate) {
  EXPECT_TRUE(valid_skill_name("deploy-webapp_2"));
  EXPECT_TRUE(valid_skill_name("A"));
  EXPECT_FALSE(valid_skill_name(""));
  EXPECT_FALSE(valid_skill_name("../escape"));
  EXPECT_FALSE(valid_skill_name("a/b"));
  EXPECT_FALSE(valid_skill_name("a\\b"));
  EXPECT_FALSE(valid_skill_name("a b"));
  EXPECT_FALSE(valid_skill_name("dot.name"));
  EXPECT_FALSE(valid_skill_name(std::string(65, 'a')));   // length cap 64
}

TEST(SkillsScan, ScansSortedAndSkipsBadEntries) {
  const std::string root = ::testing::TempDir() + "/skills_scan_" + std::to_string(::getpid());
  fs::remove_all(root);
  write_skill(root, "zeta", "---\ndescription: last\n---\nz");
  write_skill(root, "alpha", "---\ndescription: first\n---\na");
  write_skill(root, "broken", "no frontmatter");            // skipped: unparseable
  fs::create_directories(root + "/empty-dir");              // skipped: no SKILL.md
  auto v = scan_skills_dir(root);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].name, "alpha");
  EXPECT_EQ(v[0].description, "first");
  EXPECT_EQ(v[1].name, "zeta");
}

TEST(SkillsScan, MissingDirYieldsEmpty) {
  EXPECT_TRUE(scan_skills_dir("/nonexistent/skills/dir").empty());
}

TEST(SkillsScan, FormatAnnounce) {
  EXPECT_EQ(format_skills_announce({}), "");
  std::string a = format_skills_announce({{"alpha", "first"}, {"zeta", "last"}});
  EXPECT_EQ(a,
            "Available skills (call use_skill with a name to load its full instructions):\n"
            "- alpha: first\n"
            "- zeta: last");
}

TEST(SkillsScan, ResolveSkillsDir) {
  EXPECT_EQ(resolve_skills_dir(Block{}), "skills");
  Block b; b.kv["dir"] = "my/skilldir";
  EXPECT_EQ(resolve_skills_dir(b), "my/skilldir");
}
```

- [ ] **Step 2: Add to CMake and run — expect FAIL (missing header/impl).** In `CMakeLists.txt`, after the `src/memory/store.cpp` line (~line 35), add:

```cmake
target_sources(hades_core PRIVATE src/skills/scan.cpp)
```

and after the `tests/test_session_turns.cpp` line add:

```cmake
target_sources(hades_tests PRIVATE tests/test_skills_scan.cpp)
```

Run: `nix develop --command cmake --build build` → compile error (no such header).

- [ ] **Step 3: Implement.** `include/hades/skills/scan.h`:

```cpp
// include/hades/skills/scan.h — skills library scan: frontmatter parse + roster announce
//
// Pure helpers behind the SkillsModule and the skills wiring. A skill is skills/<name>/SKILL.md
// with a "---"-fenced frontmatter carrying a one-line `description:`; the DIRECTORY name is the
// canonical skill id (frontmatter `name:` is display-only). valid_skill_name is header-only so
// the standalone tool binaries (use_skill/save_skill) share the exact same security-critical
// validation without linking hades_core.
#pragma once
#include <cctype>
#include <string>
#include <vector>
#include "hades/config.h"
namespace hades {

struct SkillInfo {
  std::string name;         // directory name — the canonical id use_skill resolves
  std::string description;  // frontmatter description — one announce line
};

// Strict skill-name gate: 1..64 chars of [A-Za-z0-9_-]. Anything else (path separators, dots,
// whitespace, empty) is rejected — a traversal name would be an arbitrary read/write escape.
inline bool valid_skill_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  return true;
}

// Extract the frontmatter `description:` from a SKILL.md. Returns "" when the text has no
// leading "---" fence, no closing fence, or no description line (tolerant, never throws).
std::string parse_skill_description(const std::string& text);

// Scan a skills dir: one SkillInfo per subdirectory whose SKILL.md yields a non-empty
// description, sorted by name (deterministic announce). Missing/unreadable dir -> {}.
std::vector<SkillInfo> scan_skills_dir(const std::string& dir);

// Render the announce block posted as SKILLS_ANNOUNCE. Empty list -> "" (no block injected).
std::string format_skills_announce(const std::vector<SkillInfo>& skills);

// Resolve the skills dir from a `Skills { dir = ... }` block (empty/absent key -> "skills").
// Single source of truth for the module AND the tool argv wiring.
std::string resolve_skills_dir(const Block& skills_cfg);

}  // namespace hades
```

`src/skills/scan.cpp`:

```cpp
// src/skills/scan.cpp — skills dir scan + frontmatter parse (pure, tolerant, never throws)
#include "hades/skills/scan.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
namespace hades {
namespace {
std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

std::string parse_skill_description(const std::string& text) {
  if (text.rfind("---", 0) != 0) return "";                 // must open with a fence
  const std::size_t first_nl = text.find('\n');
  if (first_nl == std::string::npos) return "";
  const std::size_t close = text.find("\n---", first_nl);   // closing fence line
  if (close == std::string::npos) return "";
  std::istringstream fm(text.substr(first_nl + 1, close - first_nl - 1));
  std::string line;
  while (std::getline(fm, line))
    if (line.rfind("description:", 0) == 0) return trim(line.substr(12));
  return "";
}

std::vector<SkillInfo> scan_skills_dir(const std::string& dir) {
  std::vector<SkillInfo> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec), end;
  if (ec) return out;                                       // missing dir is not an error
  for (; it != end; it.increment(ec)) {
    if (ec) break;                                          // unreadable continuation: keep what we have
    std::error_code dec;
    if (!it->is_directory(dec) || dec) continue;
    std::ifstream f(it->path() / "SKILL.md");
    if (!f) continue;                                       // not a skill dir
    std::stringstream s;
    s << f.rdbuf();
    std::string desc = parse_skill_description(s.str());
    if (desc.empty()) continue;                             // unparseable skill: skip, never crash
    out.push_back({it->path().filename().string(), std::move(desc)});
  }
  std::sort(out.begin(), out.end(),
            [](const SkillInfo& a, const SkillInfo& b) { return a.name < b.name; });
  return out;
}

std::string format_skills_announce(const std::vector<SkillInfo>& skills) {
  if (skills.empty()) return "";
  std::string out = "Available skills (call use_skill with a name to load its full instructions):";
  for (const auto& s : skills) out += "\n- " + s.name + ": " + s.description;
  return out;
}

std::string resolve_skills_dir(const Block& skills_cfg) {
  auto it = skills_cfg.kv.find("dir");
  return (it != skills_cfg.kv.end() && !it->second.empty()) ? it->second : "skills";
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SkillsScan` → all pass. Then full suite.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/skills/scan.h src/skills/scan.cpp tests/test_skills_scan.cpp CMakeLists.txt
git commit -m "feat: skills scan library (frontmatter parse, name gate, announce format)"
```

---

## Task 2: `use_skill` native tool

**Files:**
- Create: `tools/use_skill_main.cpp`
- Test: `tests/test_use_skill_tool.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hades::valid_skill_name` from `hades/skills/scan.h` (header-only, no core link).
- Produces: binary `hades-use-skill`; native protocol `{"call":"describe"|"use_skill","args":{"name":...}}` → one JSON line; skills dir = `argv[1]` (fallback `"skills"`). Success result: `{"name":..., "content":<full SKILL.md>}`.

- [ ] **Step 1: Write the failing tests** `tests/test_use_skill_tool.cpp`:

```cpp
// tests/test_use_skill_tool.cpp — drive the hades-use-skill binary over the native protocol
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

static std::string mk_skill_root() {
  const std::string root = ::testing::TempDir() + "/use_skill_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root + "/greet");
  std::ofstream f(root + "/greet/SKILL.md");
  f << "---\nname: greet\ndescription: how to greet\n---\nSay hello twice.\n";
  return root;
}

TEST(UseSkillTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({USE_SKILL_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "use_skill");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "name") != required.end());
}

TEST(UseSkillTool, ReturnsFullSkillContent) {
  const std::string root = mk_skill_root();
  nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", "greet"}}}};
  ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "greet");
  EXPECT_NE(j["result"].value("content", "").find("Say hello twice."), std::string::npos);
  EXPECT_NE(j["result"].value("content", "").find("description: how to greet"),
            std::string::npos);   // full file, frontmatter included
}

TEST(UseSkillTool, MissingSkillIsNotOk) {
  const std::string root = mk_skill_root();
  nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", "ghost"}}}};
  ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(UseSkillTool, TraversalNameFailsClosed) {
  const std::string root = mk_skill_root();
  // A "name" that would escape the skills dir must be rejected by the name gate,
  // NOT resolved as a path (arbitrary-file-read escape otherwise).
  for (const std::string bad : {"../greet", "a/b", "..", ".hidden", "a b"}) {
    nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", bad}}}};
    ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << bad;
    EXPECT_FALSE(j.value("ok", true)) << bad;
  }
}

TEST(UseSkillTool, NonStringNameIsNotOkAndDoesNotCrash) {
  ProcResult r = run_subprocess({USE_SKILL_BIN}, R"({"call":"use_skill","args":{"name":42}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt` after the `hades-pin-fact` block (~line 98) add:

```cmake
add_executable(hades-use-skill tools/use_skill_main.cpp)
target_link_libraries(hades-use-skill PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-use-skill PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(hades_tests PRIVATE tests/test_use_skill_tool.cpp)
target_compile_definitions(hades_tests PRIVATE USE_SKILL_BIN="$<TARGET_FILE:hades-use-skill>")
add_dependencies(hades_tests hades-use-skill)
```

(`target_include_directories` lets the standalone binary include `hades/skills/scan.h` for the inline name gate — header only, links nothing from core.)

- [ ] **Step 3: Implement** `tools/use_skill_main.cpp`:

```cpp
// tools/use_skill_main.cpp — bundled use_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"use_skill","args":{name}}) and writes one JSON
// line. Loads <skills_dir>/<name>/SKILL.md (skills dir = argv[1], fallback "skills") and
// returns its full content; the Arbiter loops it back to the LLM as a tool result, so the
// skill's instructions persist in the conversation. The skill NAME is strictly validated
// (valid_skill_name, shared header) — a traversal name would otherwise be an arbitrary-file-
// read escape. Fail-closed: malformed/adversarial input returns ok:false, never throws.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/skills/scan.h"   // valid_skill_name (header-only; no core link)

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "skills";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    nlohmann::json required = nlohmann::json::array();
    required.push_back("name");
    out = {{"ok", true},
           {"result",
            {{"name", "use_skill"},
             {"description",
              "Load one of your skills: returns the named skill's full SKILL.md instructions "
              "from your skills library. Call this before doing a task a skill covers."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"name", {{"type", "string"}}}}},
               {"required", required}}}}}};
  } else if (call == "use_skill") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    const bool has_name = args.contains("name") && args["name"].is_string();
    const std::string name = has_name ? args["name"].get<std::string>() : "";
    if (!hades::valid_skill_name(name)) {
      out = {{"ok", false}, {"result", {{"error", "invalid skill name"}}}};
    } else {
      std::ifstream f(dir + "/" + name + "/SKILL.md");
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "no such skill: " + name}}}};
      } else {
        std::stringstream s;
        s << f.rdbuf();
        out = {{"ok", true}, {"result", {{"name", name}, {"content", s.str()}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump() << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `-R UseSkillTool` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/use_skill_main.cpp tests/test_use_skill_tool.cpp CMakeLists.txt
git commit -m "feat: use_skill native tool (load a skill's SKILL.md, name-gated)"
```

---

## Task 3: `save_skill` native tool

**Files:**
- Create: `tools/save_skill_main.cpp`
- Test: `tests/test_save_skill_tool.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hades::valid_skill_name` (same header-only include as Task 2).
- Produces: binary `hades-save-skill`; call `save_skill` args `{name, description, body}` (all required strings) → writes `<dir>/<name>/SKILL.md` atomically (temp + rename), overwrite = update. Success result `{"saved":true,"name":...,"path":...}`.

- [ ] **Step 1: Write the failing tests** `tests/test_save_skill_tool.cpp`:

```cpp
// tests/test_save_skill_tool.cpp — drive the hades-save-skill binary over the native protocol
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

static std::string fresh_root() {
  const std::string root = ::testing::TempDir() + "/save_skill_" + std::to_string(::getpid());
  fs::remove_all(root);
  return root;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
static nlohmann::json save(const std::string& root, const std::string& name,
                           const std::string& desc, const std::string& body) {
  nlohmann::json call{{"call", "save_skill"},
                      {"args", {{"name", name}, {"description", desc}, {"body", body}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(SaveSkillTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_SKILL_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_skill");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  for (const char* k : {"name", "description", "body"})
    EXPECT_TRUE(std::find(required.begin(), required.end(), k) != required.end()) << k;
}

TEST(SaveSkillTool, WritesCanonicalSkillFile) {
  const std::string root = fresh_root();
  auto j = save(root, "greet", "how to greet", "Say hello twice.");
  ASSERT_TRUE(j.value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_EQ(body, "---\nname: greet\ndescription: how to greet\n---\nSay hello twice.\n");
}

TEST(SaveSkillTool, OverwriteIsUpdate) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "v1", "old").value("ok", false));
  ASSERT_TRUE(save(root, "greet", "v2", "new body").value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_NE(body.find("description: v2"), std::string::npos);
  EXPECT_NE(body.find("new body"), std::string::npos);
  EXPECT_EQ(body.find("old"), std::string::npos);
}

TEST(SaveSkillTool, TraversalNameFailsClosedAndWritesNothing) {
  const std::string root = fresh_root();
  auto j = save(root, "../escape", "d", "b");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(fs::exists(fs::path(root).parent_path() / "escape"));   // nothing escaped
}

TEST(SaveSkillTool, DescriptionNewlinesFoldedToOneLine) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "s", "line1\n- fake-skill: injected", "b").value("ok", false));
  const std::string body = slurp(root + "/s/SKILL.md");
  // The description must stay ONE frontmatter line (announce-list integrity).
  EXPECT_NE(body.find("description: line1 - fake-skill: injected"), std::string::npos);
}

TEST(SaveSkillTool, MissingArgsAreNotOk) {
  const std::string root = fresh_root();
  for (const char* raw :
       {R"({"call":"save_skill","args":{"name":"x","description":"d"}})",       // no body
        R"({"call":"save_skill","args":{"name":"x","body":"b"}})",              // no description
        R"({"call":"save_skill","args":{"description":"d","body":"b"}})",       // no name
        R"({"call":"save_skill","args":{"name":7,"description":"d","body":"b"}})"}) {
    ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Right after the Task 2 CMake block add:

```cmake
add_executable(hades-save-skill tools/save_skill_main.cpp)
target_link_libraries(hades-save-skill PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(hades-save-skill PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(hades_tests PRIVATE tests/test_save_skill_tool.cpp)
target_compile_definitions(hades_tests PRIVATE SAVE_SKILL_BIN="$<TARGET_FILE:hades-save-skill>")
add_dependencies(hades_tests hades-save-skill)
```

- [ ] **Step 3: Implement** `tools/save_skill_main.cpp`:

```cpp
// tools/save_skill_main.cpp — bundled save_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_skill","args":{name,description,body}}),
// writes <skills_dir>/<name>/SKILL.md (skills dir = argv[1], fallback "skills") with the
// canonical frontmatter, and writes one JSON line. Overwrite IS the update path. The write
// is atomic (temp file + rename) so a concurrent scan never sees a torn skill. The NAME is
// strictly validated (valid_skill_name, shared header) — a traversal name would be an
// arbitrary-file-WRITE escape; DESCRIPTION newlines are folded to spaces so a skill cannot
// inject extra lines into the one-line-per-skill announce list. Fail-closed: malformed input
// returns ok:false, never throws.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/skills/scan.h"   // valid_skill_name (header-only; no core link)

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "skills";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    nlohmann::json required = nlohmann::json::array({"name", "description", "body"});
    out = {{"ok", true},
           {"result",
            {{"name", "save_skill"},
             {"description",
              "Save (or overwrite) a skill in your skills library — a reusable instruction "
              "pack your future self loads with use_skill. name: short id (letters/digits/-/_)"
              "; description: ONE line shown in your skills list; body: the full markdown "
              "instructions."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"description", {{"type", "string"}}},
                 {"body", {{"type", "string"}}}}},
               {"required", required}}}}}};
  } else if (call == "save_skill") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    auto str = [&](const char* k) {
      return args.contains(k) && args[k].is_string() ? args[k].get<std::string>()
                                                     : std::string{};
    };
    const std::string name = str("name");
    std::string desc = str("description");
    const std::string body = str("body");
    if (!hades::valid_skill_name(name)) {
      out = {{"ok", false}, {"result", {{"error", "invalid skill name"}}}};
    } else if (desc.empty() || body.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: description and body required"}}}};
    } else {
      for (char& c : desc)
        if (c == '\n' || c == '\r') c = ' ';   // one skill = one announce line
      std::error_code ec;
      const std::filesystem::path skill_dir = std::filesystem::path(dir) / name;
      std::filesystem::create_directories(skill_dir, ec);   // best-effort; ofstream reports failure
      const std::string path = (skill_dir / "SKILL.md").string();
      const std::string tmp = path + ".tmp";
      std::ofstream f(tmp, std::ios::trunc);
      if (f) {
        f << "---\nname: " << name << "\ndescription: " << desc << "\n---\n" << body;
        if (body.back() != '\n') f << "\n";
        f.close();
      }
      if (!f) {
        std::remove(tmp.c_str());
        out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
      } else {
        std::filesystem::rename(tmp, path, ec);   // atomic on POSIX; replaces existing
        if (ec) {
          std::remove(tmp.c_str());
          out = {{"ok", false}, {"result", {{"error", "cannot save: " + path}}}};
        } else {
          out = {{"ok", true}, {"result", {{"saved", true}, {"name", name}, {"path", path}}}};
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump() << std::endl;
  return 0;
}
```

(`body.back()` is safe — the empty-body case already returned above.)

- [ ] **Step 4: Build + test.** `-R SaveSkillTool` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/save_skill_main.cpp tests/test_save_skill_tool.cpp CMakeLists.txt
git commit -m "feat: save_skill native tool (agent-authored skills, atomic write, name-gated)"
```

---

## Task 4: `SkillsModule`

**Files:**
- Create: `include/hades/module/skills_module.h`, `src/module/skills_module.cpp`
- Test: `tests/test_skills_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `scan_skills_dir` / `format_skills_announce` / `resolve_skills_dir` (Task 1); bus keys `TOOL_REQUEST {id, tool, args}`, `TOOL_RESULT {id, ok, content}`.
- Produces: `class SkillsModule : public Module` — `type() == "skills"`, `on_start(const Block& cfg, Blackboard&)` (reads `dir` via `resolve_skills_dir`), `on_attach(Blackboard&)` (posts `SKILLS_ANNOUNCE` string; refreshes after a successful `save_skill`).

- [ ] **Step 1: Write the failing tests** `tests/test_skills_module.cpp`:

```cpp
// tests/test_skills_module.cpp — SkillsModule: announce at attach, event-driven rescan
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/blackboard.h"
#include "hades/module/skills_module.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_root(const char* tag) {
  const std::string root =
      ::testing::TempDir() + "/skmod_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}
static void write_skill(const std::string& root, const std::string& name,
                        const std::string& desc) {
  fs::create_directories(root + "/" + name);
  std::ofstream f(root + "/" + name + "/SKILL.md");
  f << "---\ndescription: " << desc << "\n---\nbody\n";
}
static void attach(SkillsModule& m, Blackboard& bb, const std::string& dir) {
  Block cfg;
  cfg.kv["dir"] = dir;
  m.on_start(cfg, bb);
  m.on_attach(bb);
}
static std::string announce(Blackboard& bb) {
  auto e = bb.get("SKILLS_ANNOUNCE");
  return (e && e->value.is_string()) ? e->value.get<std::string>() : "<missing>";
}

TEST(SkillsModule, PostsAnnounceOnAttach) {
  const std::string root = fresh_root("attach");
  write_skill(root, "beta", "second");
  write_skill(root, "alpha", "first");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  const std::string a = announce(bb);
  EXPECT_NE(a.find("Available skills"), std::string::npos);
  EXPECT_LT(a.find("- alpha: first"), a.find("- beta: second"));   // sorted
}

TEST(SkillsModule, EmptyDirPostsEmptyString) {
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, fresh_root("empty"));
  EXPECT_EQ(announce(bb), "");
}

TEST(SkillsModule, RescansAfterSuccessfulSaveSkill) {
  const std::string root = fresh_root("rescan");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  ASSERT_EQ(announce(bb), "");
  write_skill(root, "fresh", "just saved");   // what the save_skill tool would have written
  bb.post("TOOL_REQUEST", {{"id", "s1"}, {"tool", "save_skill"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "s1"}, {"ok", true}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_NE(announce(bb).find("- fresh: just saved"), std::string::npos);
}

TEST(SkillsModule, NoRescanOnOtherToolsOrFailedSave) {
  const std::string root = fresh_root("norescan");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  write_skill(root, "sneaky", "should not appear yet");
  // other tool succeeds -> no rescan
  bb.post("TOOL_REQUEST", {{"id", "f1"}, {"tool", "fs_read"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "f1"}, {"ok", true}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_EQ(announce(bb), "");
  // save_skill FAILS -> no rescan
  bb.post("TOOL_REQUEST", {{"id", "s2"}, {"tool", "save_skill"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "s2"}, {"ok", false}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_EQ(announce(bb), "");
}

TEST(SkillsModule, MalformedBusPayloadsDoNotCrash) {
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, fresh_root("malformed"));
  bb.post("TOOL_REQUEST", "not an object", "x");
  bb.post("TOOL_RESULT", 42, "x");
  bb.pump();   // must not throw
  SUCCEED();
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add near the `memory_module` lines (~line 126):

```cmake
target_sources(hades_core PRIVATE src/module/skills_module.cpp)
target_sources(hades_tests PRIVATE tests/test_skills_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/skills_module.h`:

```cpp
// include/hades/module/skills_module.h — skills roster app (MOOS behavior-library analogue)
//
// Announces the skills library on the bus: scans the skills dir at attach and posts
// SKILLS_ANNOUNCE (a preformatted block the Arbiter folds into the leading system message via
// get() — latest-value, no subscription). Event-driven refresh: tracks in-flight save_skill
// TOOL_REQUEST ids and rescans when the matching TOOL_RESULT lands ok — NO per-turn scanning.
// Empty/missing dir -> posts "" (the Arbiter injects nothing; feature costs zero when unused).
#pragma once
#include <set>
#include <string>
#include "hades/module.h"
namespace hades {
class Blackboard;
class SkillsModule : public Module {
public:
  std::string type() const override { return "skills"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

private:
  void post_announce_();
  std::string dir_ = "skills";
  Blackboard* bb_ = nullptr;
  std::set<std::string> pending_saves_;   // in-flight save_skill TOOL_REQUEST ids
};
}  // namespace hades
```

`src/module/skills_module.cpp`:

```cpp
// src/module/skills_module.cpp — scan skills dir, post SKILLS_ANNOUNCE, rescan on save_skill
#include "hades/module/skills_module.h"
#include "hades/blackboard.h"
#include "hades/skills/scan.h"
namespace hades {

void SkillsModule::on_start(const Block& cfg, Blackboard&) { dir_ = resolve_skills_dir(cfg); }

void SkillsModule::post_announce_() {
  std::string ann;
  try {
    ann = format_skills_announce(scan_skills_dir(dir_));
  } catch (...) {
    ann.clear();   // a scan failure must never crash the pump thread; degrade to "no skills"
  }
  bb_->post("SKILLS_ANNOUNCE", ann, "skills");
}

void SkillsModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // post() updates the latest-value map immediately (pump is only for handlers), so the very
  // first start_turn already sees this announce regardless of attach/pump ordering.
  post_announce_();
  bb.subscribe("TOOL_REQUEST", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    if (e.value.value("tool", "") != "save_skill") return;
    const std::string id = e.value.value("id", "");
    if (!id.empty()) pending_saves_.insert(id);
  });
  bb.subscribe("TOOL_RESULT", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    if (pending_saves_.erase(e.value.value("id", "")) == 0) return;   // not a save_skill result
    if (e.value.value("ok", false)) post_announce_();
  });
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R SkillsModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/skills_module.h src/module/skills_module.cpp tests/test_skills_module.cpp CMakeLists.txt
git commit -m "feat: SkillsModule — announce skills roster on the bus, rescan on save_skill"
```

---

## Task 5: Arbiter folds `SKILLS_ANNOUNCE` into the leading system message

**Files:**
- Modify: `src/arbiter/arbiter.cpp` (inside `Arbiter::start_turn()`, directly after the core-memory fold — currently lines ~183-189)
- Test: `tests/test_arbiter.cpp` (append)

**Interfaces:**
- Consumes: bus key `SKILLS_ANNOUNCE` (string) via `bb_->get()` — latest-value read, NO new subscription, NO new Arbiter member/setter.
- Produces: the announce appended to the leading `{role:system}` content (after SOUL/USER + core memory, `\n\n`-joined). Key absent / non-string / empty → unchanged behavior.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (match the file's existing style):

```cpp
TEST(Arbiter, SkillsAnnounceFoldedIntoLeadingSystemMessage) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL TEXT");
  a.on_attach(bb);
  bb.post("SKILLS_ANNOUNCE",
          "Available skills (call use_skill with a name to load its full instructions):\n"
          "- greet: how to greet", "skills");
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  ASSERT_FALSE(req.is_null());
  const auto& msgs = req["messages"];
  ASSERT_EQ(msgs[0]["role"], "system");
  const std::string sys = msgs[0]["content"].get<std::string>();
  EXPECT_NE(sys.find("SOUL TEXT"), std::string::npos);
  EXPECT_NE(sys.find("- greet: how to greet"), std::string::npos);
  EXPECT_LT(sys.find("SOUL TEXT"), sys.find("Available skills"));   // announce after SOUL
}

TEST(Arbiter, EmptyOrMissingSkillsAnnounceInjectsNothing) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL TEXT");
  a.on_attach(bb);
  bb.post("SKILLS_ANNOUNCE", "", "skills");   // module present, empty library
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_EQ(sys.find("Available skills"), std::string::npos);
  EXPECT_EQ(sys, "SOUL TEXT");   // nothing appended for the empty announce
}

TEST(Arbiter, NonStringSkillsAnnounceIsIgnored) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL TEXT");
  a.on_attach(bb);
  bb.post("SKILLS_ANNOUNCE", 42, "skills");   // malformed: must not throw, must not inject
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  EXPECT_EQ(req["messages"][0]["content"].get<std::string>(), "SOUL TEXT");
}
```

- [ ] **Step 2: Run — expect FAIL** (announce not in system message).
- [ ] **Step 3: Implement.** In `src/arbiter/arbiter.cpp`, inside `start_turn()`, insert right AFTER the core-memory fold block (`if (!memory_path_.empty()) { ... }`) and BEFORE `if (!sys.empty()) messages.push_back(...)`:

```cpp
  // Skills roster: fold the SkillsModule's SKILLS_ANNOUNCE (latest-value; posted at attach,
  // refreshed after a successful save_skill) into the same leading system message. Key absent,
  // non-string, or empty -> no block (a roster without the skills module costs nothing).
  if (auto sk = bb_->get("SKILLS_ANNOUNCE"); sk && sk->value.is_string()) {
    const std::string ann = sk->value.get<std::string>();
    if (!ann.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += ann;
    }
  }
```

- [ ] **Step 4: Build + test.** `-R Arbiter` → all pass (new + existing); full suite green.
- [ ] **Step 5: Commit.**

```bash
git add src/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter folds SKILLS_ANNOUNCE into the leading system message"
```

---

## Task 6: Capability table — `SkillRead` / `SkillWrite`

**Files:**
- Modify: `include/hades/objective/capability_policy.h` (enum, ~line 17), `src/objective/capability_policy.cpp` (`capability_of` ~line 149, `veto` switch ~line 204)
- Test: `tests/test_capability_policy.cpp` (append)

**Interfaces:**
- Produces: `Capability::SkillRead` / `Capability::SkillWrite` enum values; `capability_of("use_skill") == SkillRead`, `capability_of("save_skill") == SkillWrite`; both → `allow()` in `CapabilityPolicy::veto`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_capability_policy.cpp` (match its existing style; add `#include "hades/blackboard.h"` if absent):

```cpp
TEST(CapabilityPolicy, SkillToolsHaveDistinctCapabilities) {
  EXPECT_EQ(CapabilityPolicy::capability_of("use_skill"), Capability::SkillRead);
  EXPECT_EQ(CapabilityPolicy::capability_of("save_skill"), Capability::SkillWrite);
}

TEST(CapabilityPolicy, SkillToolsAreAllowedWithoutConfirm) {
  CapabilityScope sc;              // defaults: confirm_unscoped = true — proves these are NOT
  CapabilityPolicy p(sc);          // falling through to the Unknown->confirm path
  Blackboard bb;
  Action use{Action::Kind::ToolCall};
  use.tool = "use_skill";
  use.args = {{"name", "greet"}};
  EXPECT_FALSE(p.veto(bb, use).vetoed);
  Action save{Action::Kind::ToolCall};
  save.tool = "save_skill";
  save.args = {{"name", "greet"}, {"description", "d"}, {"body", "b"}};
  EXPECT_FALSE(p.veto(bb, save).vetoed);
}
```

- [ ] **Step 2: Run — expect FAIL** (compile error: no `SkillRead`).
- [ ] **Step 3: Implement.** Header enum becomes:

```cpp
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SkillRead, SkillWrite, Unknown };
```

In `capability_of`, before the final `return Capability::Unknown;`:

```cpp
  if (tool == "use_skill")                               return Capability::SkillRead;
  if (tool == "save_skill")                              return Capability::SkillWrite;
```

In the `veto` switch, before `case Capability::Unknown:`:

```cpp
    case Capability::SkillRead:
    case Capability::SkillWrite:
      // The agent's own skills library: the directory is fixed by wiring argv (never chosen by
      // the LLM) and the skill name is strictly gated in the tools. pin_fact precedent —
      // unconfirmed writes to the agent's own files; a saved skill is WEAKER than a pin (its
      // body only enters context on an explicit use_skill). Distinct capabilities (not
      // MemoryAppend) keep the table honest so a future policy can confirm-gate SkillWrite
      // without code changes.
      return allow();
```

- [ ] **Step 4: Build + test.** `-R CapabilityPolicy` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/objective/capability_policy.h src/objective/capability_policy.cpp tests/test_capability_policy.cpp
git commit -m "feat: capability table entries for use_skill/save_skill (SkillRead/SkillWrite allow)"
```

---

## Task 7: Wiring — `Agent.skills`, roster factory, skills-dir argv

**Files:**
- Modify: `app/agent_wiring.h` (Agent member + include), `app/agent_wiring.cpp` (wire_agent + Manifest overload)
- Test: `tests/test_skills_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SkillsModule` (T4), `resolve_skills_dir` (T1), tool binaries (T2/T3).
- Produces: `Agent::skills` (`std::unique_ptr<SkillsModule>`, declared after `embedding`, before `chat` — `executor` stays LAST); roster factory `"skills"`; `Skills` manifest block; resolved skills dir appended to `use_skill`/`save_skill` argv; whitespace dir → `MalConfig`. Test overload `build_agent(bb, provider, ...)` leaves `a.skills == nullptr` (embedding precedent — existing tests unchanged).

- [ ] **Step 1: Write the failing tests** `tests/test_skills_wiring.cpp`:

```cpp
// tests/test_skills_wiring.cpp — manifest-path wiring: skills module + tools end-to-end.
// Roster deliberately has NO llm module: LLM_REQUEST is captured straight off the bus, so no
// provider/api key is needed; ToolRunner executes the real save_skill binary synchronously.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_root(const char* tag) {
  const std::string root =
      ::testing::TempDir() + "/skwire_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}
static void write_skill(const std::string& root, const std::string& name,
                        const std::string& desc) {
  fs::create_directories(root + "/" + name);
  std::ofstream f(root + "/" + name + "/SKILL.md");
  f << "---\ndescription: " << desc << "\n---\nbody\n";
}
static std::string manifest_text(const std::string& dir) {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = skills\n" +
         "Module = arbiter\n" +
         "Tool = use_skill { native = " + USE_SKILL_BIN + " }\n" +
         "Tool = save_skill { native = " + SAVE_SKILL_BIN + " }\n" +
         "Skills\n{\n  dir = " + dir + "\n}\n";
}

TEST(SkillsWiring, AnnounceReachesLlmRequestSystemMessage) {
  const std::string root = fresh_root("announce");
  write_skill(root, "greet", "how to greet");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(root));
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.skills, nullptr);
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  const auto& msgs = req->value["messages"];
  ASSERT_EQ(msgs[0]["role"], "system");
  EXPECT_NE(msgs[0]["content"].get<std::string>().find("- greet: how to greet"),
            std::string::npos);
}

TEST(SkillsWiring, SaveSkillRoundTripRefreshesAnnounceAndWritesConfiguredDir) {
  const std::string root = fresh_root("roundtrip");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(root));
  Agent agent = build_agent(bb, m);
  // Drive the REAL save_skill binary through the ToolRunner (argv carries the resolved dir).
  bb.post("TOOL_REQUEST",
          {{"id", "c9"},
           {"tool", "save_skill"},
           {"args", {{"name", "newskill"}, {"description", "fresh"}, {"body", "steps"}}}},
          "arbiter");
  bb.pump();
  EXPECT_TRUE(fs::exists(root + "/newskill/SKILL.md"));   // argv append worked
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  EXPECT_NE(req->value["messages"][0]["content"].get<std::string>().find("- newskill: fresh"),
            std::string::npos);   // announce refreshed the same session
}

TEST(SkillsWiring, WhitespaceSkillsDirThrowsMalConfig) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text("/tmp/has space"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(SkillsWiring, NoSkillsRosterLeavesAgentSkillsNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.skills, nullptr);              // no coupling: absent module is simply absent
  EXPECT_FALSE(bb.get("SKILLS_ANNOUNCE").has_value());
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add after the `test_embedding_wiring` line (~131):

```cmake
target_sources(hades_tests PRIVATE tests/test_skills_wiring.cpp)
```

(The `USE_SKILL_BIN`/`SAVE_SKILL_BIN` compile definitions and dependencies exist from T2/T3.) Expect compile FAIL: `Agent` has no member `skills`.

- [ ] **Step 3: Implement.** In `app/agent_wiring.h`: add `#include "hades/module/skills_module.h"` with the other module includes, and in `struct Agent` after the `embedding` member:

```cpp
  std::unique_ptr<SkillsModule> skills;   // optional skills-roster app (announce + refresh)
```

(`executor` MUST remain the last member — do not move it.)

In `app/agent_wiring.cpp`:

1. Add `#include "hades/skills/scan.h"` (for `resolve_skills_dir`).
2. Extend `wire_agent`'s signature with a trailing parameter: `const Block& skills_cfg = Block{}` (after `session_path`).
3. Inside `wire_agent`, after the `core_path` resolution/reject_ws lines, add:

```cpp
  // Skills dir: same single-source-of-truth pattern as the memory paths — the module scans the
  // SAME dir the tools' argv points at. Resolved once here; whitespace would split the argv.
  const std::string skills_dir = resolve_skills_dir(skills_cfg);
  reject_ws(skills_dir, "skills dir");
```

4. In the `tools_resolved` loop, add a branch after the `pin_fact` one:

```cpp
    else if ((t.name == "use_skill" || t.name == "save_skill") && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + skills_dir;
```

5. After the `2c` embedding block, add:

```cpp
  // 2d) SkillsModule: posts SKILLS_ANNOUNCE at attach. The Arbiter reads it via get()
  //     (latest-value updates on post, before any pump), so ordering vs the Arbiter is not
  //     load-bearing — kept before it for consistency with the other posting modules.
  if (a.skills) {
    a.skills->on_start(skills_cfg, bb);
    a.skills->on_attach(bb);
  }
```

6. In the Manifest overload: register the factory with the others:

```cpp
  launcher.register_factory("skills",      []{ return std::make_unique<SkillsModule>(); });
```

take it with the others:

```cpp
  a.skills  = take_as<SkillsModule>(launcher, "skills");
```

extract the block next to the Memory/Embedding ones:

```cpp
  const auto skills_blocks = m.of("Skills");
  const Block skills_cfg = skills_blocks.empty() ? Block{} : skills_blocks.front();
```

and pass it as the new last argument of the `wire_agent` call:

```cpp
  wire_agent(a, bb, s, m.of("Tool"), m.of("Objective"), memory, model, embedding, session_path,
             skills_cfg);
```

The TEST overload's `wire_agent(...)` call is unchanged (default `Block{}` → dir `"skills"`; `a.skills` stays null like `a.embedding`).

- [ ] **Step 4: Build + test.** `-R SkillsWiring` → pass; **full suite** green (existing wiring/pantler tests untouched).
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp tests/test_skills_wiring.cpp CMakeLists.txt
git commit -m "feat: wire skills — Agent.skills, roster factory, Skills block, skills-dir argv"
```

---

## Task 8: Ship — dev.hades, soul.md, CLAUDE.md, lock tests

**Files:**
- Modify: `manifests/dev.hades`, `prompts/soul.md`, `CLAUDE.md`
- Possibly modify: any test asserting the shipped manifest's roster (`grep -rn "DEV_MANIFEST" tests/` — currently `tests/test_webui.cpp` holds the compile def)

**Note:** `manifests/dev.hades` carries the user's intentional uncommitted edits (`model = gpt-5.5`, `reindex_interval_s = 40`). Per the user's decision, this task commits the file **as-is plus the skills additions** — do NOT revert those values. Do NOT stage `memory/facts.md`.

- [ ] **Step 1: dev.hades.** Add `Module = skills` after `Module = serve`; add the two Tool lines after `Tool = pin_fact`; add the Skills block after the `Memory` block:

```
Module = skills
```

```
Tool = use_skill  { native = ./build/hades-use-skill }
Tool = save_skill { native = ./build/hades-save-skill }
```

```
Skills
{
  dir = skills
}
```

- [ ] **Step 2: soul.md.** Append after the memory section:

```markdown
## Skills

You have a skills library — reusable instruction packs stored on disk. The "Available skills"
list in this prompt (when present) is that library. Call `use_skill` with a skill's name to
load its full instructions BEFORE doing a task it covers. Skills may bundle scripts; run those
with the `shell` tool exactly as the skill instructs. When you work out a reusable procedure —
or the user teaches you one — distill it with `save_skill`: pick a clear name, and write the
one-line description so your future self picks the right skill from the list.
```

- [ ] **Step 3: Lock tests.** Run `grep -rn "DEV_MANIFEST" tests/` and the full suite; if any test asserts the dev.hades roster/blocks (e.g. in `tests/test_webui.cpp`), update its expectations for the new `Module = skills` + 2 Tool + `Skills` blocks. The manifest must parse with **zero fatal warnings** (`enforce_manifest`).
- [ ] **Step 4: CLAUDE.md.** Update: current-state line (add skills to the feature list + new test count), targets line (add `hades-{use-skill,save-skill}`), a short `### Skills system` section (SkillsModule announce + get() fold + 2 gated tools + zero coupling + `skills/` is git-tracked like `memory/facts.md` — agent-authored skills show as working-tree churn to review/commit), and mark item (1) of `## NEXT` done.
- [ ] **Step 5: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green.
- [ ] **Step 6: Commit.**

```bash
git add manifests/dev.hades prompts/soul.md CLAUDE.md tests/
git commit -m "feat: ship skills in dev.hades + soul.md skills guidance + docs"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 251 baseline + ~25 new, all green.
2. Manual live smoke (Vaios, needs `HADES_API_KEY`):
   ```bash
   nix develop --command ./build/hades manifests/dev.hades
   # user> save a skill for how to greet me: always answer in Greek first
   #   -> agent calls save_skill; skills/<name>/SKILL.md appears
   # user> what skills do you have?
   #   -> lists the new skill (announce refreshed same session)
   # user> use your greeting skill and greet me
   #   -> agent calls use_skill, follows the loaded instructions
   # restart -> skill still announced (persistent)
   ```
3. Security spot-check: ask the agent to `use_skill` with name `../secrets` → tool returns `ok:false` (name gate), no read outside `skills/`.
4. `hades-scope session.log SKILLS_` shows the announce posts.

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus per `feedback_sdd_implementer_opus`), per-task review, final whole-branch review, then finishing-a-development-branch (merge ff to main — no remote, never push). After approval, save this plan to `docs/superpowers/plans/2026-07-02-skills-system.md` and commit before executing.
