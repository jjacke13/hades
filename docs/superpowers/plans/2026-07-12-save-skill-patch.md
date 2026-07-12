# save_skill Patch Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a patch mode to the `save_skill` native tool so the agent edits part of an existing SKILL.md with `old_string`/`new_string` instead of resending the whole body (token-cheap skill self-improvement — Hermes borrow).

**Architecture:** One tool binary grows a second mode. Mode select by which optional arg is non-empty (`body` ⇒ save, `old_string` ⇒ patch; empty string = absent). Patch = read live file, exact-substring match EXACTLY ONCE, replace, then refuse unless the result still parses as a skill (`parse_skill_description` non-empty — the same function the scanner uses, so "valid" means "won't vanish from the announce roster"). Atomic tmp+rename. Zero changes to SkillsModule (rescan keys on `tool=="save_skill"` + `ok:true` — patch fires it free), capability table (SkillWrite → allow), or wiring (skills dir stays argv[1]).

**Tech Stack:** C++20, nlohmann_json, GoogleTest, CMake+Ninja inside `nix develop`.

Spec: `docs/superpowers/specs/2026-07-12-save-skill-patch-design.md` (committed `8d9de55`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build` then `nix develop --command ctest --test-dir build --output-on-failure`. Baseline **614/614 green** before Task 1.
- Branch `feat/save-skill-patch` (already created; spec committed). Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- **Empty string = absent** for ALL optional args (the schedule_task `833b9aa` rule): `body:""` does not select save mode, `old_string:""` does not select patch mode, `description:""` in patch mode is not an error.
- **No `replace_all`** in patch mode (YAGNI — skill files are small; ambiguity fix = more surrounding context).
- **No staleness `expect_version`** (v1, documented in the spec): the match-once check runs against live disk content.
- Exact error strings as written in the code blocks below (tests assert substrings of them).
- Never stage `manifests/dev.hades`, `manifests/pi.hades`, `memory/facts.md` (user's live uncommitted files).
- File headers: `// path — one-line purpose` + short explanation block (house style).

---

## File Structure

```
tools/save_skill_main.cpp        T1  full rewrite of the tool (two modes)
tests/test_save_skill_tool.cpp   T1  1 modified test + 12 new tests
CMakeLists.txt                   T1  one line: compile src/skills/scan.cpp into hades-save-skill
docs/manifest-reference.md       T2  patch-mode sentence in the Skills block section
prompts/soul.md                  T2  one sentence in ## Skills
CLAUDE.md                        T2  item 5b shipped + test count
```

---

## Task 1: Patch mode in the save_skill tool

**Files:**
- Modify: `tools/save_skill_main.cpp` (full content below)
- Modify: `tests/test_save_skill_tool.cpp` (one existing test changes, 12 appended)
- Modify: `CMakeLists.txt` (~line 111 block)

**Interfaces:**
- Consumes: `hades::valid_skill_name` (inline, `include/hades/skills/scan.h`) and `hades::parse_skill_description(const std::string&) -> std::string` (declared in the same header, DEFINED in `src/skills/scan.cpp` — the tool does not link `hades_core`, so `src/skills/scan.cpp` is compiled into the tool target; the cron_store precedent).
- Produces: `save_skill` native protocol v2 — describe schema `required: ["name"]`, properties `name/description/body/old_string/new_string` (all strings). Save result `{"saved":true,"name":…,"path":…}` (unchanged). Patch result `{"patched":true,"name":…,"path":…}`.

- [ ] **Step 1: Baseline.** Run the full suite; expect 614/614 green:

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
```

- [ ] **Step 2: Write the failing tests.** In `tests/test_save_skill_tool.cpp`:

**(a) REPLACE the existing `DescribeYieldsSpec` test** (the schema's `required` shrinks to `["name"]` — mode args are conditionally required, enforced at runtime):

```cpp
TEST(SaveSkillTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_SKILL_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_skill");
  // Only name is unconditionally required: body selects save mode, old_string selects patch
  // mode, and the runtime dispatch enforces the per-mode arg sets.
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  ASSERT_EQ(required.size(), 1u);
  EXPECT_EQ(required[0], "name");
  const auto& props = j["result"]["schema"]["properties"];
  for (const char* k : {"name", "description", "body", "old_string", "new_string"})
    EXPECT_TRUE(props.contains(k)) << k;
}
```

**(b) APPEND after `MissingArgsAreNotOk`** — first a patch helper (sends empty `description`/`body` alongside the patch args, deliberately mimicking a weak LLM that fills every schema field, so these tests also pin empty-string=absent):

```cpp
// Patch-mode driver: fills EVERY schema field (empty strings for the unused mode's args) the
// way weak LLMs do — so every patch test also pins the empty-string-is-absent rule.
static nlohmann::json patch(const std::string& root, const std::string& name,
                            const std::string& olds, const std::string& news,
                            const std::string& desc = "") {
  nlohmann::json call{{"call", "save_skill"},
                      {"args",
                       {{"name", name},
                        {"description", desc},
                        {"body", ""},
                        {"old_string", olds},
                        {"new_string", news}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(SaveSkillTool, PatchReplacesExactlyOnce) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "how to greet", "Say hello twice.\nThen wave.").value("ok", false));
  auto j = patch(root, "greet", "hello twice", "hi once");
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"].value("patched", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_NE(body.find("Say hi once."), std::string::npos);
  EXPECT_EQ(body.find("hello twice"), std::string::npos);
  EXPECT_NE(body.find("description: how to greet"), std::string::npos);   // frontmatter intact
}

TEST(SaveSkillTool, PatchEmptyNewStringDeletes) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "Say hello twice.\nThen wave.\n").value("ok", false));
  ASSERT_TRUE(patch(root, "greet", "\nThen wave.", "").value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_EQ(body.find("Then wave"), std::string::npos);
  EXPECT_NE(body.find("Say hello twice."), std::string::npos);
}

TEST(SaveSkillTool, PatchOldStringNotFoundFailsAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "never there", "x");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchAmbiguousMatchFailsAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "alpha beta alpha").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "alpha", "gamma");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("2 times"), std::string::npos);
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, BothModesFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body").value("ok", false));
  nlohmann::json call{{"call", "save_skill"},
                      {"args",
                       {{"name", "greet"}, {"description", "d"}, {"body", "new body"},
                        {"old_string", "body"}, {"new_string", "x"}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("not both"), std::string::npos);
}

TEST(SaveSkillTool, NeitherModeFails) {
  const std::string root = fresh_root();
  // Every field present but empty = every field absent (weak-LLM shape).
  auto j = patch(root, "greet", "", "");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveSkillTool, PatchWithDescriptionFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "body text", "new text", "a new description");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchBreakingFrontmatterRefusedAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "how to greet", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  // Patching the description KEY away would make the scanner drop the skill -> refuse.
  auto j = patch(root, "greet", "description:", "junk:");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("frontmatter"), std::string::npos);
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchCanEditFrontmatterDescriptionValue) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "old description", "body text").value("ok", false));
  // Editing the description VALUE (key intact) is legal patch usage.
  ASSERT_TRUE(patch(root, "greet", "description: old description",
                    "description: better one-liner").value("ok", false));
  EXPECT_NE(slurp(root + "/greet/SKILL.md").find("description: better one-liner"),
            std::string::npos);
}

TEST(SaveSkillTool, PatchMissingSkillFails) {
  const std::string root = fresh_root();
  auto j = patch(root, "ghost", "a", "b");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("no such skill"), std::string::npos);
}

TEST(SaveSkillTool, PatchTraversalNameFailsClosed) {
  const std::string root = fresh_root();
  auto j = patch(root, "../escape", "a", "b");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveSkillTool, PatchIdenticalStringsFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body").value("ok", false));
  auto j = patch(root, "greet", "body", "body");
  EXPECT_FALSE(j.value("ok", true));
}
```

- [ ] **Step 3: CMake — compile the scan lib into the tool.** In `CMakeLists.txt`, change the `hades-save-skill` block (currently at ~line 111) from:

```cmake
add_executable(hades-save-skill tools/save_skill_main.cpp)
```

to:

```cmake
# parse_skill_description is defined in src/skills/scan.cpp; the tool does not link hades_core,
# so it compiles the scan TU into itself (cron_store precedent) — the patch-mode validity check
# uses the SAME parse the scanner uses, so "valid" == "won't vanish from the announce roster".
add_executable(hades-save-skill tools/save_skill_main.cpp src/skills/scan.cpp)
```

- [ ] **Step 4: Run tests — expect FAIL.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SaveSkillTool
```

Expected: the new patch tests fail (`ok:false` missing-arg errors / `patched` absent); `DescribeYieldsSpec` fails on `required.size()`.

- [ ] **Step 5: Implement.** Replace `tools/save_skill_main.cpp` with:

```cpp
// tools/save_skill_main.cpp — bundled save_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_skill","args":{name,description,body,
// old_string,new_string}}) and writes one JSON line. TWO modes on <skills_dir>/<name>/SKILL.md
// (skills dir = argv[1], fallback "skills"), selected by which optional arg is non-empty
// (empty string = absent — weak LLMs fill every schema field):
//   SAVE  (body non-empty):       canonical frontmatter + body; overwrite IS the update path.
//   PATCH (old_string non-empty): exact-substring replace, must match EXACTLY ONCE; the
//     patched file must still parse as a skill (parse_skill_description non-empty — the SAME
//     parse the scanner runs) or the patch is refused, so the agent cannot brick a skill out
//     of its own announce roster. No staleness expect_version (v1): a stale old_string fails
//     the match against LIVE disk content and the error says to re-read — self-healing.
// Both modes write ATOMICALLY (temp + rename) so a concurrent scan never sees a torn skill.
// The NAME is strictly validated (valid_skill_name, shared header) — a traversal name would be
// an arbitrary-file-WRITE escape; DESCRIPTION newlines are folded to spaces so a skill cannot
// inject extra lines into the one-line-per-skill announce list. Fail-closed: malformed input
// returns ok:false, never throws.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/skills/scan.h"   // valid_skill_name (inline) + parse_skill_description (TU compiled in)

namespace {

// Atomic write shared by both modes. Returns "" on success, else the error message.
std::string write_atomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
  if (f) {
    f << content;
    f.close();
  }
  if (!f) {
    std::remove(tmp.c_str());
    return "cannot write: " + path;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);   // atomic on POSIX; replaces existing
  if (ec) {
    std::remove(tmp.c_str());
    return "cannot save: " + path;
  }
  return {};
}

}  // namespace

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
    out = {{"ok", true},
           {"result",
            {{"name", "save_skill"},
             {"description",
              "Save or patch a skill in your skills library — a reusable instruction pack "
              "your future self loads with use_skill. TWO modes: (1) SAVE — send name + "
              "description (ONE line shown in your skills list) + body (the full markdown "
              "instructions) to create or overwrite a skill; (2) PATCH — send name + "
              "old_string + new_string to edit part of an EXISTING skill without resending "
              "the whole body; old_string must match exactly once (give more surrounding "
              "context if ambiguous). Use one mode per call, never both."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"description", {{"type", "string"}}},
                 {"body", {{"type", "string"}}},
                 {"old_string", {{"type", "string"}}},
                 {"new_string", {{"type", "string"}}}}},
               {"required", nlohmann::json::array({"name"})}}}}}};
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
    const std::string olds = str("old_string");
    const std::string news = str("new_string");
    auto fail = [&](const std::string& msg) {
      out = {{"ok", false}, {"result", {{"error", msg}}}};
    };
    // Mode select: empty string = absent (weak LLMs fill every schema field).
    const bool save_mode = !body.empty();
    const bool patch_mode = !olds.empty();
    if (!hades::valid_skill_name(name)) {
      fail("invalid skill name");
    } else if (save_mode && patch_mode) {
      fail("provide body (save) OR old_string/new_string (patch), not both");
    } else if (!save_mode && !patch_mode) {
      fail("provide body (to save a full skill) or old_string/new_string (to patch an existing one)");
    } else if (save_mode) {
      if (desc.empty()) {
        fail("missing arg: description and body required");
      } else {
        for (char& c : desc)
          if (c == '\n' || c == '\r') c = ' ';   // one skill = one announce line
        std::error_code ec;
        const std::filesystem::path skill_dir = std::filesystem::path(dir) / name;
        std::filesystem::create_directories(skill_dir, ec);   // best-effort; write reports failure
        const std::string path = (skill_dir / "SKILL.md").string();
        std::string content = "---\nname: " + name + "\ndescription: " + desc + "\n---\n" + body;
        if (body.back() != '\n') content += "\n";
        const std::string err = write_atomic(path, content);
        if (!err.empty())
          fail(err);
        else
          out = {{"ok", true}, {"result", {{"saved", true}, {"name", name}, {"path", path}}}};
      }
    } else {   // patch mode
      if (!desc.empty()) {
        fail("patch edits the file directly — put the description change in old_string/new_string");
      } else if (olds == news) {
        fail("old_string and new_string are identical");
      } else {
        const std::string path = (std::filesystem::path(dir) / name / "SKILL.md").string();
        std::ifstream f(path, std::ios::binary);
        if (!f) {
          fail("no such skill: " + name + " — create it first with a full save (body)");
        } else {
          std::stringstream ss;
          ss << f.rdbuf();
          std::string content = ss.str();
          f.close();
          // Count non-overlapping occurrences (edit_file contract: exactly one, no replace_all
          // — skill files are small; the ambiguity fix is more surrounding context).
          int count = 0;
          for (std::size_t pos = content.find(olds); pos != std::string::npos;
               pos = content.find(olds, pos + olds.size()))
            ++count;
          if (count == 0) {
            fail("old_string not found in skill '" + name +
                 "' — use_skill it to see its current content and retry");
          } else if (count > 1) {
            fail("old_string matches " + std::to_string(count) +
                 " times — give more surrounding context");
          } else {
            content.replace(content.find(olds), olds.size(), news);
            // The scanner's own parse is the validity oracle: if it can no longer extract a
            // description, the skill would silently vanish from the announce roster.
            if (hades::parse_skill_description(content).empty()) {
              fail("patch would break the skill's frontmatter — fix old_string/new_string or "
                   "resend the full skill with body");
            } else {
              const std::string err = write_atomic(path, content);
              if (!err.empty())
                fail(err);
              else
                out = {{"ok", true},
                       {"result", {{"patched", true}, {"name", name}, {"path", path}}}};
            }
          }
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  // replace-handler dump: never let an invalid-UTF-8 byte in a message throw and kill the tool
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
```

- [ ] **Step 6: Build + test.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SaveSkillTool
```

Expected: all `SaveSkillTool.*` pass (6 existing (incl. the modified Describe) + 12 new). Then the FULL suite:

```bash
nix develop --command ctest --test-dir build --output-on-failure
```

Expected: **626/626** (614 baseline + 12 net-new tests; `DescribeYieldsSpec` modified in place).

- [ ] **Step 7: Commit.**

```bash
git add tools/save_skill_main.cpp tests/test_save_skill_tool.cpp CMakeLists.txt
git commit -m "feat: save_skill patch mode — old_string/new_string edit with match-once + frontmatter brick-guard"
```

---

## Task 2: Ship docs — manifest-reference, soul.md, CLAUDE.md

**Files:**
- Modify: `docs/manifest-reference.md` (the `Skills` block section, the paragraph after the key table — currently around line 322)
- Modify: `prompts/soul.md` (the `## Skills` section, currently lines 44-51)
- Modify: `CLAUDE.md` (Skills v2 idea-list item 5b + test count in the Current state header)

**Interfaces:**
- Consumes: the Task 1 behavior exactly as implemented (mode select by non-empty arg, match-once, frontmatter brick-guard, no replace_all, `required: ["name"]`).
- Produces: nothing downstream — docs only.

- [ ] **Step 1: manifest-reference.** In `docs/manifest-reference.md`, find the paragraph in the `Skills` block section:

```
`dir` is appended to the `use_skill`/`save_skill` tool argv (single source of truth) — keep it
whitespace-free. Skill names are gated to `[A-Za-z0-9_-]{1,64}` in the tools (no path traversal).
The dir is git-tracked in this repo (the agent writes skills at runtime → working-tree churn).
```

Append this paragraph directly after it:

```
**`save_skill` has two modes** (selected by which optional arg is non-empty; an empty string
counts as absent): a full **save** (`name` + `description` + `body` — creates or overwrites) and
a **patch** (`name` + `old_string`/`new_string` — edits part of an EXISTING skill without
resending the body; `old_string` must match exactly once, no `replace_all`). A patch that would
break the frontmatter (the scanner could no longer parse a description) is refused with the file
untouched, so the agent cannot brick a skill out of its own roster. Sending `body` and
`old_string` together, or a `description` alongside a patch, is an error.
```

- [ ] **Step 2: soul.md.** In `prompts/soul.md` `## Skills`, the paragraph currently ends:

```
or the user teaches you one — distill it with `save_skill`: pick a clear name, and write the
one-line description so your future self picks the right skill from the list.
```

Change the ending to:

```
or the user teaches you one — distill it with `save_skill`: pick a clear name, and write the
one-line description so your future self picks the right skill from the list. To refine an
existing skill, patch it instead of resending the whole body: call `save_skill` with just the
name and an `old_string`/`new_string` replacement.
```

- [ ] **Step 3: CLAUDE.md.** Two edits:

(a) In the **Skills v2 idea-list**, item 5b currently reads:

```
5b. **`save_skill` patch mode (Hermes borrow, small):** an `old_string`/`new_string` patch action
   (edit_file-style) so the agent refines a skill incrementally instead of resending the whole
   body — token-cheap skill self-improvement (Hermes `skill_manage patch`, 2026-07-11 research).
   Pairs with the soul.md learn-triggers.
```

Replace with:

```
5b. ~~`save_skill` patch mode~~ — **SHIPPED 2026-07-12** (`feat/save-skill-patch`): save_skill
   gained optional `old_string`/`new_string` (empty=absent mode select; match EXACTLY ONCE, no
   replace_all; post-patch frontmatter validation via the scanner's own `parse_skill_description`
   → a patch that would brick the skill out of the roster is refused, file untouched; atomic
   write; rescan/capability/wiring unchanged — rescan keys on tool name + ok, SkillWrite still
   allow, dir still argv[1]). No staleness expect_version (v1): stale old_string fails the
   live-content match and the error says to re-read. `src/skills/scan.cpp` is compiled into the
   tool binary (cron_store precedent). Spec:
   `docs/superpowers/specs/2026-07-12-save-skill-patch-design.md`.
```

(b) In the `## Current state` header line, update the test count `**614/614 tests**` → `**626/626 tests**` (both occurrences if the TSan note repeats it — TSan was not re-run for this feature; leave the TSan phrasing tied to 614 and write `**626/626 tests** (ASan+UBSan; TSan 614/614 as of feat/simplex — no threads touched since)`).

- [ ] **Step 4: Verify docs claims + suite still green.**

```bash
nix develop --command ctest --test-dir build --output-on-failure
```

Expected: 626/626. Also confirm the shipped manifest still parses (no manifest changes were made — this is a no-op check that nothing was accidentally staged):

```bash
git status --short   # must NOT list manifests/dev.hades, manifests/pi.hades, memory/facts.md as staged
```

- [ ] **Step 5: Commit.**

```bash
git add docs/manifest-reference.md prompts/soul.md CLAUDE.md
git commit -m "docs: save_skill patch mode — manifest-reference, soul.md guidance, CLAUDE.md 5b shipped"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: **626/626** green.
2. Manual protocol smoke (no LLM needed):

```bash
mkdir -p /tmp/skpatch/greet && printf -- '---\nname: greet\ndescription: how to greet\n---\nSay hello twice.\n' > /tmp/skpatch/greet/SKILL.md
echo '{"call":"save_skill","args":{"name":"greet","description":"","body":"","old_string":"hello twice","new_string":"hi once"}}' | ./build/hades-save-skill /tmp/skpatch
# -> {"ok":true,"result":{"name":"greet","patched":true,...}}
cat /tmp/skpatch/greet/SKILL.md   # body now "Say hi once."
echo '{"call":"save_skill","args":{"name":"greet","description":"","body":"","old_string":"description:","new_string":"junk:"}}' | ./build/hades-save-skill /tmp/skpatch
# -> ok:false "patch would break the skill's frontmatter…"
```

3. Live smoke (Vaios, later, needs `HADES_API_KEY`): ask the agent to tweak one line of an
   existing skill — expect a `save_skill` call with `old_string`/`new_string`, a one-line diff in
   `skills/<name>/SKILL.md`, announce unchanged.

## Execution

Subagent-driven development (house process): fresh implementer per task, per-task review, final
whole-branch review, then finishing-a-development-branch (merge ff to main — no remote, never
push). Baseline 614/614 before Task 1.
