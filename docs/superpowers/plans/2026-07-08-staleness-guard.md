# hades Staleness Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Protect `edit_file`/`write_file` from lost updates: a file changed on disk since the LLM last observed it is refused (file untouched) with a self-healing "re-read and retry" error.

**Architecture:** Tools report a content-hash `version` in their results (`fs_read` = bytes read; `edit_file`/`write_file` = bytes written). The Arbiter keeps a `canonical-path → version` map, harvested from tool results, and injects `expect_version` into `edit_file`/`write_file` args at dispatch (stripping any LLM-supplied value first). The tool enforces the check inside its own subprocess, right next to the atomic write. No LLM cooperation, no confirm prompts, no config keys; no record → no injection → today's behavior (staleness only).

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, std::filesystem.

## Global Constraints

- **Every build/test runs inside `nix develop`:** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **511/511 green**. Each task keeps the whole suite green.
- Branch `feat/staleness-guard` (created; spec committed `8440b94`). Spec: `docs/superpowers/specs/2026-07-08-staleness-guard-design.md`.
- Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By, NO Generated-with line.**
- **Hash is exactly:** FNV-1a 64-bit over raw bytes → 16 lowercase hex chars. `file_version("")` == `"cbf29ce484222325"` (the FNV offset basis). Header-only inline in `include/hades/tool/file_version.h` — tool binaries include it WITHOUT linking `hades_core` (the `valid_skill_name` precedent).
- **Refusal error text is exactly:** `"file changed on disk since you last read it — fs_read it again and retry"` (both tools; tests match on the substring `"changed on disk"`).
- **`expect_version` is NOT added to any `describe` schema** — it is Arbiter-injected plumbing, invisible to the LLM.
- **Canonical map key:** `std::filesystem::path(p).lexically_normal().string()` — lexical only (symlinks unresolved, documented v1 parity with the capability model).
- **On refusal the target file is byte-identical** (edit_file already never writes before its checks; write_file gains atomic tmp+rename).
- Working-tree hygiene: `manifests/dev.hades`, `manifests/pi.hades`, `memory/facts.md` carry the user's uncommitted LIVE config — **never stage them**; never `git add -A`. (This feature ships no manifest changes at all.)
- Tool `main`s never throw on adversarial input (existing guarded-parse pattern stays).

---

## File Structure

```
include/hades/tool/file_version.h       T1  inline FNV-1a 64 -> 16-hex (shared, header-only)
tools/fs_read_main.cpp                  T1  result gains version (modify)
tests/test_file_version.cpp             T1
tests/test_tools.cpp                    T1  fs_read version assertion (append)
tools/edit_file_main.cpp                T2  expect_version check + version in result (modify)
tools/write_file_main.cpp               T2  expect_version check + version + atomic write (modify)
tests/test_edit_file_tool.cpp           T2  (append)
tests/test_tools.cpp                    T2  write_file cases (append)
include/hades/arbiter.h                 T3  file_versions_ + pending_file_ops_ (modify)
src/apps/arbiter/arbiter.cpp            T3  strip+inject at dispatch, harvest on result (modify)
tests/test_arbiter.cpp                  T3  (append)
tests/test_staleness_e2e.cpp            T4  real binaries end-to-end
CLAUDE.md, docs/manifest-reference.md   T4  docs (modify)
CMakeLists.txt                          T1,T4 (add test sources)
```

---

## Task 1: `file_version.h` + `fs_read` reports a version

**Files:**
- Create: `include/hades/tool/file_version.h`, `tests/test_file_version.cpp`
- Modify: `tools/fs_read_main.cpp`, `tests/test_tools.cpp` (append one test), `CMakeLists.txt`

**Interfaces — Produces:**
- `inline std::string hades::file_version(const std::string& bytes)` — FNV-1a 64 → 16 lowercase hex.
- `fs_read` success result: `{"content": <str>, "version": <16-hex>}`.

- [ ] **Step 1: Write the failing tests.** Create `tests/test_file_version.cpp`:

```cpp
// tests/test_file_version.cpp — the shared content-hash behind the staleness guard
#include <gtest/gtest.h>
#include <string>
#include "hades/tool/file_version.h"
using namespace hades;

TEST(FileVersion, KnownEmptyHash) {
  // FNV-1a 64 offset basis — pins the algorithm (a change breaks every stored expectation).
  EXPECT_EQ(file_version(""), "cbf29ce484222325");
}

TEST(FileVersion, DeterministicAndFormat) {
  const std::string v = file_version("hello world\n");
  EXPECT_EQ(v.size(), 16u);
  EXPECT_EQ(v, file_version("hello world\n"));                 // stable
  for (char c : v) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
}

TEST(FileVersion, OneByteChangeChangesHash) {
  EXPECT_NE(file_version("aaaa"), file_version("aaab"));
  EXPECT_NE(file_version("x"), file_version(""));
}
```

Append to `tests/test_tools.cpp` (it already has `call_tool` + `FS_READ_BIN`; match its existing style — check the helper's exact signature before writing):

```cpp
TEST(Tools, FsReadReportsContentVersion) {
  const std::string path = ::testing::TempDir() + "/ver_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "the content\n"; }
  auto j = call_tool(FS_READ_BIN, "fs_read", {{"path", path}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("content", ""), "the content\n");
  EXPECT_EQ(j["result"].value("version", ""), hades::file_version("the content\n"));
}
```

(If `test_tools.cpp` has no `call_tool` helper with that exact shape, adapt to the file's real helper — the assertion set is the requirement, not the helper name. Add `#include "hades/tool/file_version.h"` to the test file.)

- [ ] **Step 2: CMake + build — expect FAIL.** In `CMakeLists.txt`, next to the other test sources add:

```cmake
target_sources(hades_tests PRIVATE tests/test_file_version.cpp)
```

Also ensure `hades-fs-read` can include hades headers: if its `add_executable` block has no `target_include_directories(hades-fs-read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)`, add it (the `hades-use-skill` precedent). Build → FAIL (missing header).

- [ ] **Step 3: Implement.** Create `include/hades/tool/file_version.h`:

```cpp
// include/hades/tool/file_version.h — content-hash version token for the staleness guard
//
// FNV-1a 64-bit over the raw bytes, rendered as 16 lowercase hex chars. fs_read stamps the bytes
// it returned; edit_file/write_file stamp the bytes they wrote and verify an Arbiter-injected
// expect_version before writing. Header-only so the standalone tool binaries share the exact same
// hash without linking hades_core. Detects ACCIDENTAL concurrent modification (lost updates) —
// not an adversary; collision resistance is a non-goal.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
namespace hades {
inline std::string file_version(const std::string& bytes) {
  std::uint64_t h = 1469598103934665603ULL;                 // FNV offset basis
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ULL;                                  // FNV prime
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}
}  // namespace hades
```

In `tools/fs_read_main.cpp`: add `#include "hades/tool/file_version.h"`, and change the success branch to:

```cpp
        std::stringstream s;
        s << f.rdbuf();
        const std::string content = s.str();
        out = {{"ok", true},
               {"result", {{"content", content}, {"version", hades::file_version(content)}}}};
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R "FileVersion|Tools"` → pass; then the FULL suite → green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/tool/file_version.h tools/fs_read_main.cpp tests/test_file_version.cpp tests/test_tools.cpp CMakeLists.txt
git commit -m "feat: file_version content hash; fs_read reports version (staleness guard part 1)"
```

---

## Task 2: `edit_file` + `write_file` enforce `expect_version`

**Files:**
- Modify: `tools/edit_file_main.cpp`, `tools/write_file_main.cpp`, `tests/test_edit_file_tool.cpp` (append), `tests/test_tools.cpp` (append), possibly `CMakeLists.txt` (include dirs for the two binaries)

**Interfaces:**
- Consumes: `hades::file_version` (T1 header).
- Produces: both tools accept optional arg `expect_version` (string; NOT in the describe schema). Mismatch → `ok:false`, error containing `"changed on disk"`, file byte-identical. Success results gain `"version"` = hash of the newly written content. `write_file` becomes atomic (tmp + rename, mode preserved).

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_edit_file_tool.cpp` (reuse its existing `run_edit`/helper style — check the file's helpers first and match them):

```cpp
TEST(EditFileTool, ExpectVersionMatchAllowsEdit) {
  const std::string path = ::testing::TempDir() + "/ev_ok_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "alpha beta gamma"; }
  nlohmann::json args{{"path", path}, {"old_string", "beta"}, {"new_string", "BETA"},
                      {"expect_version", hades::file_version("alpha beta gamma")}};
  ProcResult r = run_subprocess({EDIT_FILE_BIN},
      nlohmann::json{{"call", "edit_file"}, {"args", args}}.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("version", ""), hades::file_version("alpha BETA gamma"));
}

TEST(EditFileTool, ExpectVersionMismatchRefusesUntouched) {
  const std::string path = ::testing::TempDir() + "/ev_bad_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "alpha beta gamma"; }
  nlohmann::json args{{"path", path}, {"old_string", "beta"}, {"new_string", "BETA"},
                      {"expect_version", "0000000000000000"}};   // stale token
  ProcResult r = run_subprocess({EDIT_FILE_BIN},
      nlohmann::json{{"call", "edit_file"}, {"args", args}}.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("changed on disk"), std::string::npos);
  std::ifstream f(path); std::stringstream s; s << f.rdbuf();
  EXPECT_EQ(s.str(), "alpha beta gamma");                        // byte-identical
}

TEST(EditFileTool, NoExpectVersionBehavesAsToday) {
  const std::string path = ::testing::TempDir() + "/ev_abs_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "alpha beta"; }
  nlohmann::json args{{"path", path}, {"old_string", "beta"}, {"new_string", "B"}};
  ProcResult r = run_subprocess({EDIT_FILE_BIN},
      nlohmann::json{{"call", "edit_file"}, {"args", args}}.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  EXPECT_TRUE(j.value("ok", false));
  EXPECT_FALSE(j["result"].value("version", "").empty());        // version still reported
}

TEST(EditFileTool, DescribeSchemaDoesNotMentionExpectVersion) {
  ProcResult r = run_subprocess({EDIT_FILE_BIN}, R"({"call":"describe"})", 30.0);
  EXPECT_EQ(r.out.find("expect_version"), std::string::npos);    // Arbiter plumbing, not LLM API
}
```

Append to `tests/test_tools.cpp` (write_file):

```cpp
TEST(Tools, WriteFileExpectVersionGate) {
  const std::string path = ::testing::TempDir() + "/wv_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "original"; }
  // Mismatch -> refused, untouched.
  auto bad = call_tool(WRITE_FILE_BIN, "write_file",
                       {{"path", path}, {"content", "clobber"}, {"expect_version", "0000000000000000"}});
  EXPECT_FALSE(bad.value("ok", true));
  EXPECT_NE(bad["result"].value("error", "").find("changed on disk"), std::string::npos);
  { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); EXPECT_EQ(s.str(), "original"); }
  // Match -> written, version of the NEW content reported.
  auto ok = call_tool(WRITE_FILE_BIN, "write_file",
                      {{"path", path}, {"content", "fresh"}, {"expect_version", hades::file_version("original")}});
  ASSERT_TRUE(ok.value("ok", false)) << ok.dump();
  EXPECT_EQ(ok["result"].value("version", ""), hades::file_version("fresh"));
  { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); EXPECT_EQ(s.str(), "fresh"); }
}

TEST(Tools, WriteFileExpectVersionOnDeletedFileRefuses) {
  const std::string path = ::testing::TempDir() + "/wv_gone_" + std::to_string(::getpid()) + ".txt";
  std::filesystem::remove(path);   // file was read once, then deleted externally
  auto j = call_tool(WRITE_FILE_BIN, "write_file",
                     {{"path", path}, {"content", "x"}, {"expect_version", "cbf29ce484222325"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(std::filesystem::exists(path));                   // nothing created
}

TEST(Tools, WriteFilePreservesModeOnOverwrite) {
  const std::string path = ::testing::TempDir() + "/wv_mode_" + std::to_string(::getpid()) + ".sh";
  { std::ofstream f(path, std::ios::trunc); f << "#!/bin/sh\n"; }
  ::chmod(path.c_str(), 0755);
  auto j = call_tool(WRITE_FILE_BIN, "write_file", {{"path", path}, {"content", "#!/bin/sh\necho hi\n"}});
  ASSERT_TRUE(j.value("ok", false));
  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0755));       // exec bit survives the atomic rename
}
```

(Add includes the tests need: `<sys/stat.h>`, `<filesystem>`, `"hades/tool/file_version.h"` — match what each test file already includes.)

- [ ] **Step 2: Build + run — expect FAIL** (no `expect_version` handling, write_file drops the mode / lacks `version`).

- [ ] **Step 3: Implement `edit_file`.** In `tools/edit_file_main.cpp`: add `#include "hades/tool/file_version.h"`. After `content` is slurped and `f.close()`, insert the staleness gate BEFORE the occurrence count:

```cpp
      // Staleness guard: expect_version is Arbiter-injected (never LLM-supplied; absent from the
      // describe schema). It is the hash of the file as the conversation last observed it — a
      // mismatch means someone else changed the file since; refuse so nothing is clobbered.
      const std::string expect = jstr("expect_version");
      if (!expect.empty() && hades::file_version(content) != expect) {
        out = {{"ok", false},
               {"result", {{"error",
                 "file changed on disk since you last read it — fs_read it again and retry"}}}};
        std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
        return 0;
      }
```

(If the file's control flow makes an early return awkward inside the else-chain, restructure minimally — the requirement is: check after read, before count/replace, refuse without writing.) In the success result, add the new content's version:

```cpp
            out = {{"ok", true},
                   {"result", {{"path", path}, {"replacements", done},
                               {"version", hades::file_version(content)}}}};
```

(`content` at that point holds the post-replacement text that was written.)

- [ ] **Step 4: Implement `write_file`.** In `tools/write_file_main.cpp`: add `#include <sys/stat.h>`, `#include <cstdio>`, `#include <filesystem>`, `#include <sstream>`, `#include "hades/tool/file_version.h"`. Replace the write branch with:

```cpp
    std::string path = args.value("path", "");
    std::string content = args.value("content", "");
    std::string expect = args.contains("expect_version") && args["expect_version"].is_string()
                             ? args["expect_version"].get<std::string>() : std::string{};
    if (path.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: path"}}}};
    } else if (!expect.empty()) {
      // Staleness guard (Arbiter-injected): verify the file still matches what the conversation
      // last observed. Unreadable/deleted counts as changed — the observed file is gone.
      std::ifstream cur(path, std::ios::binary);
      std::stringstream cs;
      if (cur) cs << cur.rdbuf();
      if (!cur || hades::file_version(cs.str()) != expect) {
        out = {{"ok", false},
               {"result", {{"error",
                 "file changed on disk since you last read it — fs_read it again and retry"}}}};
      }
    }
    if (out.is_null()) {
      // Atomic write (tmp + rename, edit_file pattern): a crash never leaves a torn file, and a
      // refusal above never touched it. Preserve an existing file's mode across the rename.
      const std::string tmp = path + ".tmp";
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (f) { f << content; f.close(); }
      if (!f) {
        std::remove(tmp.c_str());
        out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
      } else {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode);
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
          std::remove(tmp.c_str());
          out = {{"ok", false}, {"result", {{"error", "cannot save: " + path}}}};
        } else {
          out = {{"ok", true},
                 {"result", {{"path", path}, {"bytes_written", static_cast<int>(content.size())},
                             {"version", hades::file_version(content)}}}};
        }
      }
    }
```

(Adapt to the file's actual structure — `out.is_null()` works because `out` starts as a null json; if the surrounding chain differs, keep the semantics: gate first, atomic write second, nothing written on refusal.) Keep the describe schema UNCHANGED (no `expect_version`). CMake: add `target_include_directories(... PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)` for `hades-write-file` and `hades-edit-file` if absent.

- [ ] **Step 5: Build + test.** `-R "EditFileTool|Tools"` → pass; FULL suite green (existing write_file/edit_file tests must still pass — the no-expect_version paths are unchanged in behavior).
- [ ] **Step 6: Commit.**

```bash
git add tools/edit_file_main.cpp tools/write_file_main.cpp tests/test_edit_file_tool.cpp tests/test_tools.cpp CMakeLists.txt
git commit -m "feat: edit_file/write_file enforce expect_version; write_file atomic (staleness guard part 2)"
```

---

## Task 3: Arbiter — track versions, inject `expect_version`

**Files:**
- Modify: `include/hades/arbiter.h`, `src/apps/arbiter/arbiter.cpp`
- Test: `tests/test_arbiter.cpp` (append)

**Interfaces:**
- Consumes: `version` in tracked TOOL_RESULT contents (T1/T2).
- Produces: `file_versions_` map (canonical path → version), `pending_file_ops_` (tool-call id → canonical path); `expect_version` stripped-then-injected on `edit_file`/`write_file` dispatch (both the direct path and the confirm-approved re-post carry it — the injection happens before the veto loop, so `pending_` captures injected args).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (match its rig style: `Blackboard bb; Arbiter a; a.on_attach(bb); a.set_tools({...}); bb.post("USER_MESSAGE",...)` then `LLM_RESPONSE` with a `tool_call` object and `TOOL_RESULT` posts; capture `TOOL_REQUEST` via subscribe):

```cpp
// ---- staleness guard: version tracking + expect_version injection ----
namespace {
// Drive one fs_read round-trip that records version `ver` for `path`.
void seed_version(Blackboard& bb, const std::string& path, const std::string& ver,
                  const char* id = "r1") {
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", id}, {"name", "fs_read"}, {"arguments", {{"path", path}}}}}},
          "llm");
  bb.pump();
  bb.post("TOOL_RESULT",
          {{"id", id}, {"ok", true}, {"content", {{"content", "body"}, {"version", ver}}}},
          "tool_runner");
  bb.pump();
}
}  // namespace

TEST(Arbiter, InjectsExpectVersionFromPriorRead) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ToolSpec{"fs_read", "", {}}, ToolSpec{"edit_file", "", {}}});
  nlohmann::json req;
  bb.subscribe("TOOL_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "edit it", "chat"); bb.pump();
  seed_version(bb, "notes.txt", "aaaa111122223333");
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                          {"arguments", {{"path", "notes.txt"}, {"old_string", "a"}, {"new_string", "b"}}}}}},
          "llm");
  bb.pump();
  ASSERT_EQ(req.value("tool", ""), "edit_file");
  EXPECT_EQ(req["args"].value("expect_version", ""), "aaaa111122223333");
}

TEST(Arbiter, NoRecordNoInjectionAndHallucinatedTokenStripped) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ToolSpec{"edit_file", "", {}}});
  nlohmann::json req;
  bb.subscribe("TOOL_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "edit", "chat"); bb.pump();
  bb.post("LLM_RESPONSE",   // LLM invents an expect_version for a never-read file
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                          {"arguments", {{"path", "unseen.txt"}, {"old_string", "a"},
                                         {"new_string", "b"}, {"expect_version", "ffffffffffffffff"}}}}}},
          "llm");
  bb.pump();
  ASSERT_EQ(req.value("tool", ""), "edit_file");
  EXPECT_FALSE(req["args"].contains("expect_version"));   // stripped, nothing injected
}

TEST(Arbiter, SuccessfulEditUpdatesVersionForNextEdit) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ToolSpec{"fs_read", "", {}}, ToolSpec{"edit_file", "", {}}});
  nlohmann::json req;
  bb.subscribe("TOOL_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "go", "chat"); bb.pump();
  seed_version(bb, "f.txt", "1111111111111111");
  // First edit succeeds and reports the NEW version.
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                          {"arguments", {{"path", "f.txt"}, {"old_string", "a"}, {"new_string", "b"}}}}}},
          "llm");
  bb.pump();
  bb.post("TOOL_RESULT",
          {{"id", "e1"}, {"ok", true},
           {"content", {{"path", "f.txt"}, {"replacements", 1}, {"version", "2222222222222222"}}}},
          "tool_runner");
  bb.pump();
  // Second edit (no re-read) must carry the post-edit version — edit->edit chains work.
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "e2"}, {"name", "edit_file"},
                          {"arguments", {{"path", "f.txt"}, {"old_string", "b"}, {"new_string", "c"}}}}}},
          "llm");
  bb.pump();
  EXPECT_EQ(req["args"].value("expect_version", ""), "2222222222222222");
}

TEST(Arbiter, FailedResultDoesNotUpdateVersion) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ToolSpec{"fs_read", "", {}}, ToolSpec{"write_file", "", {}}});
  nlohmann::json req;
  bb.subscribe("TOOL_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "go", "chat"); bb.pump();
  seed_version(bb, "g.txt", "aaaaaaaaaaaaaaaa");
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "w1"}, {"name", "write_file"},
                          {"arguments", {{"path", "g.txt"}, {"content", "x"}}}}}},
          "llm");
  bb.pump();
  bb.post("TOOL_RESULT",   // refused (stale) — must NOT overwrite the recorded version
          {{"id", "w1"}, {"ok", false},
           {"content", {{"error", "file changed on disk ..."}, {"version", "bad"}}}},
          "tool_runner");
  bb.pump();
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "w2"}, {"name", "write_file"},
                          {"arguments", {{"path", "g.txt"}, {"content", "x"}}}}}},
          "llm");
  bb.pump();
  EXPECT_EQ(req["args"].value("expect_version", ""), "aaaaaaaaaaaaaaaa");   // unchanged
}

TEST(Arbiter, PathKeysAreLexicallyNormalized) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ToolSpec{"fs_read", "", {}}, ToolSpec{"edit_file", "", {}}});
  nlohmann::json req;
  bb.subscribe("TOOL_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "go", "chat"); bb.pump();
  seed_version(bb, "./sub/../h.txt", "cccccccccccccccc");   // read under an aliased spelling
  bb.post("LLM_RESPONSE",
          {{"text", ""}, {"epoch", 1},
           {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                          {"arguments", {{"path", "h.txt"}, {"old_string", "a"}, {"new_string", "b"}}}}}},
          "llm");
  bb.pump();
  EXPECT_EQ(req["args"].value("expect_version", ""), "cccccccccccccccc");   // same canonical key
}
```

(Check `ToolSpec`'s actual constructor shape in `test_arbiter.cpp` / `arbiter.h` and match it.)

- [ ] **Step 2: Build + run — expect FAIL** (no injection yet; the tests asserting `expect_version` fail).

- [ ] **Step 3: Implement.** In `include/hades/arbiter.h`, add two private members next to `peer_vars_`:

```cpp
  // Staleness guard: last-observed content version per file (canonical path -> 16-hex hash),
  // harvested from fs_read/edit_file/write_file TOOL_RESULTs; injected as expect_version into
  // edit_file/write_file dispatches. In-memory only (a restart clears it — degrade to unguarded).
  std::map<std::string, std::string> file_versions_;
  std::map<std::string, std::string> pending_file_ops_;   // tool-call id -> canonical path
```

In `src/apps/arbiter/arbiter.cpp`: add `#include <filesystem>` and a file-local helper near the top:

```cpp
namespace {
// Canonical map key for the staleness guard: lexical normalization only ("./x", "a/../x" -> "x").
// NOT realpath — symlink aliasing is a documented v1 gap, same as the capability model's canon_path.
std::string canon_file_key(const std::string& p) {
  return std::filesystem::path(p).lexically_normal().string();
}
}  // namespace
```

In `dispatch_or_gate`, make the action locally mutable and add the strip+inject BEFORE the objective loop (so the confirm path's `pending_` snapshot also carries the injection):

```cpp
void Arbiter::dispatch_or_gate(const Action& act_in, const nlohmann::json& assistant_msg) {
  Action act = act_in;
  // Staleness guard: expect_version is Arbiter-owned plumbing. Strip anything LLM-supplied
  // (a hallucinated token must never reach the tool), then inject the recorded version when
  // this file has been observed. No record -> no injection -> the tool behaves as before.
  if (act.kind == Action::Kind::ToolCall &&
      (act.tool == "edit_file" || act.tool == "write_file") && act.args.is_object()) {
    act.args.erase("expect_version");
    if (auto p = act.args.find("path"); p != act.args.end() && p->is_string()) {
      auto it = file_versions_.find(canon_file_key(p->get<std::string>()));
      if (it != file_versions_.end()) act.args["expect_version"] = it->second;
    }
  }
  ...existing body, unchanged, now reading the local `act`...
```

At BOTH `TOOL_REQUEST` post sites (the direct dispatch in `dispatch_or_gate` and the confirm-approved re-post in `on_confirm`), record the pending file op just before the post (a tiny private helper keeps it in one place):

```cpp
// In dispatch_or_gate, before bb_->post("TOOL_REQUEST", ...):
    track_file_op_(act.tool_id, act.tool, act.args);
// In on_confirm, before its bb_->post("TOOL_REQUEST", ...):
    track_file_op_(pending_.value("tool_id", ""), pending_.value("tool", ""),
                   pending_.contains("args") ? pending_["args"] : nlohmann::json::object());
```

```cpp
void Arbiter::track_file_op_(const std::string& id, const std::string& tool,
                             const nlohmann::json& args) {
  if (tool != "fs_read" && tool != "edit_file" && tool != "write_file") return;
  if (id.empty() || !args.is_object()) return;
  if (auto p = args.find("path"); p != args.end() && p->is_string())
    pending_file_ops_[id] = canon_file_key(p->get<std::string>());
}
```

(Declare `track_file_op_` in the header's private section.) In `on_tool_result`, right after `content` is extracted, harvest:

```cpp
  // Staleness guard: a successful tracked file op reports the file's new content version.
  if (auto it = pending_file_ops_.find(v.value("id", "")); it != pending_file_ops_.end()) {
    if (v.value("ok", false) && content.is_object())
      if (auto ver = content.find("version"); ver != content.end() && ver->is_string())
        file_versions_[it->second] = ver->get<std::string>();
    pending_file_ops_.erase(it);
  }
```

- [ ] **Step 4: Build + test.** `-R Arbiter` → all pass (new + every existing Arbiter test — the local-copy refactor of `dispatch_or_gate` must not change any other behavior); FULL suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/arbiter.h src/apps/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter tracks file versions, injects expect_version (staleness guard part 3)"
```

---

## Task 4: E2E with real binaries + docs + ship

**Files:**
- Create: `tests/test_staleness_e2e.cpp`
- Modify: `CMakeLists.txt`, `CLAUDE.md`, `docs/manifest-reference.md`

- [ ] **Step 1: Write the E2E test** `tests/test_staleness_e2e.cpp` (follow `tests/test_e2e.cpp`'s rig: real ToolRunner with `Block` tool configs pointing at the compiled binaries, plus an Arbiter on the same bus — check that file's exact setup and mirror it):

```cpp
// tests/test_staleness_e2e.cpp — lost-update protection end-to-end with the REAL tool binaries:
// read -> external modification -> edit REFUSED (file intact) -> re-read -> edit succeeds.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/module/tool_runner.h"
using namespace hades;

static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str();
}

TEST(StalenessE2E, ExternalChangeRefusedThenRereadSucceeds) {
  const std::string path =
      ::testing::TempDir() + "/stale_e2e_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "line one\nline two\n"; }

  Blackboard bb;
  ToolRunner tools;
  Block fs;   fs.section = "Tool"; fs.name = "fs_read";    fs.kv["native"] = FS_READ_BIN;
  Block ed;   ed.section = "Tool"; ed.name = "edit_file";  ed.kv["native"] = EDIT_FILE_BIN;
  tools.add_tool(fs); tools.add_tool(ed);
  tools.on_start(Block{}, bb); tools.on_attach(bb);
  Arbiter a; a.on_attach(bb);
  a.set_tools(tools.specs());   // match test_e2e.cpp's actual way of handing specs to the Arbiter

  nlohmann::json last_result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { last_result = e.value; });
  bb.post("USER_MESSAGE", "work on the file", "chat"); bb.pump();

  // 1) LLM reads the file (real fs_read runs; Arbiter records its version).
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "r1"}, {"name", "fs_read"}, {"arguments", {{"path", path}}}}}}, "llm");
  bb.pump();
  ASSERT_TRUE(last_result.value("ok", false));

  // 2) The file changes EXTERNALLY (another turn / a human / a heartbeat).
  { std::ofstream f(path, std::ios::trunc); f << "line one CHANGED\nline two\n"; }

  // 3) The stale edit is REFUSED and the file is untouched.
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                     {"arguments", {{"path", path}, {"old_string", "line two"}, {"new_string", "LINE 2"}}}}}}, "llm");
  bb.pump();
  EXPECT_FALSE(last_result.value("ok", true));
  EXPECT_NE(last_result["content"].value("error", "").find("changed on disk"), std::string::npos);
  EXPECT_EQ(slurp(path), "line one CHANGED\nline two\n");

  // 4) Re-read, then the same edit succeeds.
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "r2"}, {"name", "fs_read"}, {"arguments", {{"path", path}}}}}}, "llm");
  bb.pump();
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "e2"}, {"name", "edit_file"},
                     {"arguments", {{"path", path}, {"old_string", "line two"}, {"new_string", "LINE 2"}}}}}}, "llm");
  bb.pump();
  EXPECT_TRUE(last_result.value("ok", false)) << last_result.dump();
  EXPECT_EQ(slurp(path), "line one CHANGED\nLINE 2\n");
}
```

(The rig details — `ToolRunner` header path, how specs reach the Arbiter, whether `set_tools(tools.specs())` exists — MUST be checked against `tests/test_e2e.cpp` and adapted; the four-phase scenario is the requirement.)

- [ ] **Step 2: CMake + run.** Add `target_sources(hades_tests PRIVATE tests/test_staleness_e2e.cpp)`. Build; the test should pass already (Tasks 1-3 complete) — it is the integration lock. FULL suite green.

- [ ] **Step 3: Docs.**
  - `CLAUDE.md`: replace the `### edit/write staleness guard (backlog …)` section under "Other open work" with a short `### Staleness guard (shipped 2026-07-08, feat/staleness-guard)` subsection: version tokens (FNV-1a 16-hex) reported by fs_read/edit_file/write_file → Arbiter `file_versions_` map (canonical-lexical keys, in-memory, survives `/new`, cleared by restart) → `expect_version` stripped-then-injected at dispatch (both direct + confirm paths) → tool refuses with "changed on disk … re-read and retry" (self-healing, no confirm prompt — works on heartbeat/peer turns) → write_file now atomic (tmp+rename, mode-preserved). Not tracked: grep/glob/git_read/shell (next fs_read re-syncs). Update the test count.
  - `docs/manifest-reference.md`: in the `Tool` blocks section, one short paragraph: `edit_file`/`write_file` are staleness-guarded (mechanism + the exact error text operators will see in the Eventlog + "no configuration; always on").

- [ ] **Step 4: Full suite + TSan.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan --output-on-failure
```

Both green.

- [ ] **Step 5: Commit.**

```bash
git add tests/test_staleness_e2e.cpp CMakeLists.txt CLAUDE.md docs/manifest-reference.md
git commit -m "feat: staleness-guard e2e lock + docs (shipped)"
```

---

## Verification (end-to-end)

1. Full suite + TSan in `nix develop`: baseline 511 + ~15 new, all green.
2. Manual live smoke (Vaios, optional): `fs_read` a file in chat → edit it in an editor → ask the agent to `edit_file` it → refusal message → agent re-reads on its own → retry succeeds.
3. Eventlog: `hades-scope session.log` shows `expect_version` inside the `TOOL_REQUEST` args (observability for free).

## Execution

Subagent-driven development: fresh implementer per task (sonnet for T1/T2 — mechanical tool edits with complete code; opus for T3 — Arbiter dispatch/confirm-path surgery; sonnet for T4), per-task review (`cpp-reviewer`; opus for T3), fix loop for Critical/Important, final whole-branch review (opus), then finishing-a-development-branch (merge ff to `main`, no remote, never push). Plan saved at `docs/superpowers/plans/2026-07-08-staleness-guard.md`; commit before executing.
