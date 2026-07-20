# Session Compaction (compact-and-continue) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turns that fall out of the request window get folded into a rolling per-session summary by a background aux LLM call — compact-and-continue instead of today's silent truncation.

**Architecture:** The Arbiter detects window-drop in `start_turn` and posts `COMPACT_REQUEST`; an opt-in `CompactorModule` (the auto-extract pattern verbatim: merged cfg, own provider, Executor worker, throw-wrapped ALWAYS-terminal post) replies `SESSION_SUMMARY` or `COMPACT_FAILED`; the Arbiter applies it under id+monotonic guards, persists a sidecar `.hades/sessions/<id>.summary.md`, and folds the summary into the leading system message. No module rostered → nothing subscribes → byte-identical to today.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest, `hades::Executor`, `OpenAICompatProvider` + `cpr_http`.

**Spec:** `docs/superpowers/specs/2026-07-20-session-compaction-design.md` (approved, committed `c097a82` on branch `feat/compactor`).

## Global Constraints

- **Every build/test command runs inside `nix develop`.** ASan lane: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. TSan lane: same with `build-tsan`. Baseline: **774/774 both lanes**. NEVER reconfigure `build/` (its CMake cache carries `-fsanitize=address,undefined`).
- Branch `feat/compactor`. Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- Never stage: `manifests/dev.local.hades`, `manifests/pi.hades`, `manifests/dev2.hades`, `memory/`, `skills/greek-greeting/`, `skills/ponytail/`, `build*/`. `manifests/dev.hades` is the SANITIZED PUBLIC TEMPLATE.
- Exact values (spec, verbatim): span digest per-message cap **1000** bytes, whole-span cap **24000** bytes with omission marker `[... N earlier messages omitted ...]`; `summary_char_limit` default **4000**; `timeout_s` default **60**; sidecar first line `upto: N` then one blank line then markdown; fold label exactly `Earlier in this session (compacted from turns no longer shown; may be stale — re-verify files/live state before relying on a past action's result):`; bus keys `COMPACT_REQUEST` `{session, upto, span, current_summary}`, `SESSION_SUMMARY` `{session, upto, text}`, `COMPACT_FAILED` `{session, upto}`, `COMPACTED` `{upto, chars}`.
- The compactor worker is **throw-wrapped and ALWAYS posts a terminal reply** (`SESSION_SUMMARY` or `COMPACT_FAILED`) — the tool-offload BG_DONE lesson; a silent failure must never wedge the Arbiter's `pending_compact_`.
- Worker capture discipline (LLMModule/auto-extract precedent): non-owning `Provider*`/`Blackboard*` + value copies + `&busy_`; no pump-mutated field read off-thread. `Agent::compactor` sits with the plain modules (before `executor` in the member list) so the Executor joins the worker while the module is alive.
- UTF-8 byte-truncation reuses the public `hades::trunc_utf8_bytes` from `include/hades/module/tool_runner.h` (shipped in tool-offload) — do NOT write a third copy of the walk-back.
- Fail-soft floor: a roster without `Module = compactor` must leave every LLM_REQUEST byte-identical to today (existing arbiter tests are the lock; they must pass unchanged).
- Pump handlers never throw; guarded JSON access on every bus read.

---

## File Structure

```
include/hades/compact/compact.h      T1  pure helpers: digest_span, sidecar codec, paths
src/core/compact.cpp                 T1
tests/test_compact.cpp               T1
include/hades/arbiter.h              T2  members + window_start_ + on_session_summary
src/apps/arbiter/arbiter.cpp         T2  detect / apply / fold / sidecar / resume / new
tests/test_arbiter.cpp               T2  (append)
include/hades/module/compactor_module.h  T3
src/apps/compactor/compactor.cpp     T3
tests/test_compactor_module.cpp      T3  (module + full-loop integration w/ Arbiter)
app/agent_wiring.h / .cpp            T4  member, factory, Compactor merged cfg (2g)
tests/test_compactor_wiring.cpp      T4
manifests/dev.hades, prompts/soul.md, docs/manifest-reference.md, CLAUDE.md  T5
CMakeLists.txt                       T1, T3, T4 (register sources/tests)
```

---

## Task 1: Compact pure helpers

**Files:**
- Create: `include/hades/compact/compact.h`, `src/core/compact.cpp`
- Test: `tests/test_compact.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**
- `struct SidecarSummary { std::size_t upto = 0; std::string text; };`
- `nlohmann::json digest_span(const std::vector<nlohmann::json>& span, std::size_t per_msg_cap = 1000, std::size_t total_cap = 24000)` — json ARRAY of `{role, content}`.
- `SidecarSummary parse_summary_sidecar(const std::string& file_text)` — tolerant; garbage → `{0, ""}`.
- `std::string serialize_summary_sidecar(std::size_t upto, const std::string& text)`.
- `std::string sidecar_path_for(const std::string& session_path)` — `…/<id>.jsonl` → `…/<id>.summary.md`; `""` → `""`.
- `std::string session_stem(const std::string& session_path)` — filename stem; `""` → `""`.

- [ ] **Step 1: Write the failing tests** — create `tests/test_compact.cpp`:

```cpp
// tests/test_compact.cpp — pure compaction helpers: span digest, sidecar codec, paths
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/compact/compact.h"
using namespace hades;

static nlohmann::json msg(const char* role, const std::string& content) {
  return {{"role", role}, {"content", content}};
}

TEST(Compact, DigestKeepsRoleAndContent) {
  auto d = digest_span({msg("user", "hello"), msg("assistant", "hi there")});
  ASSERT_TRUE(d.is_array());
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0]["role"], "user");
  EXPECT_EQ(d[0]["content"], "hello");
  EXPECT_EQ(d[1]["content"], "hi there");
}

TEST(Compact, DigestTruncatesPerMessageOnUtf8Boundary) {
  std::string big(998, 'a');
  big += "\xCE\xB1\xCE\xB1";                       // 2 two-byte codepoints straddling the cap
  auto d = digest_span({msg("user", big)}, 1000, 24000);
  const std::string c = d[0]["content"].get<std::string>();
  EXPECT_LE(c.size(), 1000u + 12u);                // cap + " (truncated)" marker
  EXPECT_NE(c.find(" (truncated)"), std::string::npos);
  EXPECT_NO_THROW(nlohmann::json(c).dump());       // strict dump = valid UTF-8 proof
}

TEST(Compact, DigestTotalCapDropsOldestWithMarker) {
  std::vector<nlohmann::json> span;
  for (int i = 0; i < 10; ++i)
    span.push_back(msg("user", "m" + std::to_string(i) + std::string(500, 'x')));
  auto d = digest_span(span, 1000, 2000);          // room for ~3-4 newest messages
  ASSERT_GE(d.size(), 2u);
  const std::string first = d[0]["content"].get<std::string>();
  EXPECT_NE(first.find("earlier messages omitted"), std::string::npos);
  const std::string last = d[d.size() - 1]["content"].get<std::string>();
  EXPECT_NE(last.find("m9"), std::string::npos);   // newest survives
  // no dropped-message content present
  for (const auto& e : d)
    EXPECT_EQ(e["content"].get<std::string>().find("m0x"), std::string::npos);
}

TEST(Compact, DigestNamesToolCallsAndToleratesNonStringContent) {
  nlohmann::json tc = {{"role", "assistant"}, {"content", nullptr},
                       {"tool_calls", nlohmann::json::array(
                            {{{"id", "c1"}, {"type", "function"},
                              {"function", {{"name", "web_search"}, {"arguments", "{}"}}}}})}};
  auto d = digest_span({tc, {{"role", "weird"}, {"content", 42}}});
  EXPECT_NE(d[0]["content"].get<std::string>().find("web_search"), std::string::npos);
  EXPECT_FALSE(d[1]["content"].get<std::string>().empty());   // dumped, not crashed
}

TEST(Compact, SidecarRoundTripAndTolerantParse) {
  const std::string s = serialize_summary_sidecar(7, "line one\nline two");
  EXPECT_EQ(s.rfind("upto: 7\n", 0), 0u);
  const SidecarSummary p = parse_summary_sidecar(s);
  EXPECT_EQ(p.upto, 7u);
  EXPECT_EQ(p.text, "line one\nline two");
  EXPECT_EQ(parse_summary_sidecar("").upto, 0u);
  EXPECT_TRUE(parse_summary_sidecar("garbage first line\n\nbody").text.empty());
  EXPECT_TRUE(parse_summary_sidecar("upto: notanumber\n\nbody").text.empty());
}

TEST(Compact, PathsDeriveFromSessionPath) {
  EXPECT_EQ(sidecar_path_for(".hades/sessions/20260720-1.jsonl"),
            ".hades/sessions/20260720-1.summary.md");
  EXPECT_EQ(sidecar_path_for(""), "");
  EXPECT_EQ(session_stem(".hades/sessions/20260720-1.jsonl"), "20260720-1");
  EXPECT_EQ(session_stem(""), "");
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt` add next to the other `hades_core` sources: `target_sources(hades_core PRIVATE src/core/compact.cpp)` and next to the test sources: `target_sources(hades_tests PRIVATE tests/test_compact.cpp)`. Build → compile error (missing header).

- [ ] **Step 3: Implement.** `include/hades/compact/compact.h`:

```cpp
// include/hades/compact/compact.h — session-compaction pure helpers
//
// Shared between the Arbiter (detect/apply/persist) and the CompactorModule (summarize):
// digest_span turns the about-to-drop history span into a bounded json array for the aux
// request (per-message + total byte caps, oldest dropped first — archival recall still
// covers them); the sidecar codec reads/writes `.summary.md` ("upto: N" line, blank line,
// markdown); the path helpers derive the sidecar path and the session id (filename stem)
// used by the COMPACT_REQUEST/SESSION_SUMMARY session guard.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {

struct SidecarSummary {
  std::size_t upto = 0;   // history_ messages the summary covers
  std::string text;       // markdown summary ("" = no summary)
};

// Digest a history span for the aux summarize request. Each message becomes {role, content}
// with content byte-capped at per_msg_cap on a UTF-8 boundary; the whole array is capped at
// total_cap bytes by dropping the OLDEST messages first, replaced by one omission marker
// entry. Non-string content is named ("[called tool: X]") or replace-dumped — never throws.
nlohmann::json digest_span(const std::vector<nlohmann::json>& span,
                           std::size_t per_msg_cap = 1000, std::size_t total_cap = 24000);

// Tolerant sidecar parse: expects "upto: N\n\n<text>"; anything malformed -> {0, ""}.
SidecarSummary parse_summary_sidecar(const std::string& file_text);
std::string serialize_summary_sidecar(std::size_t upto, const std::string& text);

// ".hades/sessions/<id>.jsonl" -> ".hades/sessions/<id>.summary.md"; "" -> "".
std::string sidecar_path_for(const std::string& session_path);
// Session id for the compaction bus guards: the session file's stem; "" -> "".
std::string session_stem(const std::string& session_path);

}  // namespace hades
```

`src/core/compact.cpp`:

```cpp
// src/core/compact.cpp — session-compaction pure helpers (see the header)
#include "hades/compact/compact.h"
#include <filesystem>
#include <sstream>
#include "hades/module/tool_runner.h"   // trunc_utf8_bytes (public inline, tool-offload)
namespace hades {
namespace {
// One message's text for the digest: string content as-is; an assistant tool-call pair-head
// is NAMED rather than dumped (the call structure is noise to a summarizer); anything else
// replace-dumped (never throws on invalid UTF-8).
std::string msg_text(const nlohmann::json& m) {
  if (m.contains("content") && m["content"].is_string()) return m["content"].get<std::string>();
  if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty() &&
      m["tool_calls"][0].is_object()) {
    const auto& f = m["tool_calls"][0];
    return "[called tool: " +
           (f.contains("function") && f["function"].is_object()
                ? f["function"].value("name", std::string{"?"})
                : std::string{"?"}) + "]";
  }
  if (m.contains("content"))
    return m["content"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  return "";
}
}  // namespace

nlohmann::json digest_span(const std::vector<nlohmann::json>& span,
                           std::size_t per_msg_cap, std::size_t total_cap) {
  // Walk newest -> oldest, keeping messages while within total_cap; then emit oldest-first
  // with one omission marker for whatever fell off the front.
  std::vector<nlohmann::json> kept;
  std::size_t total = 0;
  for (std::size_t i = span.size(); i-- > 0;) {
    std::string c = trunc_utf8_bytes(msg_text(span[i]), per_msg_cap);
    const std::string role = span[i].is_object() ? span[i].value("role", "") : "";
    const std::size_t sz = c.size() + role.size();
    if (!kept.empty() && total + sz > total_cap) break;
    total += sz;
    kept.push_back({{"role", role}, {"content", std::move(c)}});
  }
  nlohmann::json out = nlohmann::json::array();
  const std::size_t omitted = span.size() - kept.size();
  if (omitted > 0)
    out.push_back({{"role", "system"},
                   {"content", "[... " + std::to_string(omitted) + " earlier messages omitted ...]"}});
  for (std::size_t i = kept.size(); i-- > 0;) out.push_back(std::move(kept[i]));
  return out;
}

SidecarSummary parse_summary_sidecar(const std::string& file_text) {
  SidecarSummary s;
  std::istringstream in(file_text);
  std::string first;
  if (!std::getline(in, first)) return s;
  if (first.rfind("upto: ", 0) != 0) return s;
  try {
    const long long n = std::stoll(first.substr(6));
    if (n <= 0) return s;
    s.upto = static_cast<std::size_t>(n);
  } catch (...) {
    return s;
  }
  std::string blank;
  std::getline(in, blank);   // the separator line (tolerate its absence)
  std::ostringstream rest;
  rest << in.rdbuf();
  s.text = rest.str();
  while (!s.text.empty() && s.text.back() == '\n') s.text.pop_back();
  if (s.text.empty()) s.upto = 0;   // a summary with no text is no summary
  return s;
}

std::string serialize_summary_sidecar(std::size_t upto, const std::string& text) {
  return "upto: " + std::to_string(upto) + "\n\n" + text + "\n";
}

std::string sidecar_path_for(const std::string& session_path) {
  if (session_path.empty()) return "";
  const std::filesystem::path p(session_path);
  return (p.parent_path() / (p.stem().string() + ".summary.md")).string();
}

std::string session_stem(const std::string& session_path) {
  if (session_path.empty()) return "";
  return std::filesystem::path(session_path).stem().string();
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R Compact` → 6 pass; full suite green (774 + 6 = 780).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/compact/compact.h src/core/compact.cpp tests/test_compact.cpp CMakeLists.txt
git commit -m "feat: compaction pure helpers — span digest, summary sidecar codec, paths"
```

---

## Task 2: Arbiter — detect, apply, fold, persist, resume

**Files:**
- Modify: `include/hades/arbiter.h` (members + two private methods), `src/apps/arbiter/arbiter.cpp` (`start_turn` ~187-305, `on_attach` ~74-124, `load_history` ~57-72, NEW_SESSION handler ~109-123)
- Test: `tests/test_arbiter.cpp` (append)

**Interfaces:**
- Consumes (T1): `digest_span`, `parse_summary_sidecar`, `serialize_summary_sidecar`, `sidecar_path_for`, `session_stem` from `hades/compact/compact.h`.
- Produces: posts `COMPACT_REQUEST {session, upto, span, current_summary}` (at most one in flight); handles `SESSION_SUMMARY` (guards → `summary_text_`/`summarized_upto_` → sidecar write → `COMPACTED`) and `COMPACT_FAILED` (clears `pending_compact_`); folds the summary after core memory, before skills; `load_history()` also loads the sidecar; NEW_SESSION resets all three members. Task 3's module consumes/produces the matching bus keys.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (file style; `#include "hades/compact/compact.h"` if needed for serialize in the resume test):

```cpp
TEST(Arbiter, WindowDropFiresOneCompactRequest) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_history_budget_chars(400.0);               // tiny window: old turns fall out fast
  std::vector<nlohmann::json> reqs;
  bb.subscribe("COMPACT_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  const std::string big(150, 'x');
  for (int i = 0; i < 4; ++i) {                    // 4 user+assistant pairs >> 400 chars
    bb.post("USER_MESSAGE", big + std::to_string(i), "chat"); bb.pump();
    bb.post("LLM_RESPONSE", {{"text","ok" + std::to_string(i)},{"epoch",(std::uint64_t)(i+1)}}, "llm");
    bb.pump();
  }
  ASSERT_GE(reqs.size(), 1u);
  const auto& r = reqs[0];
  EXPECT_GT(r.value("upto", 0), 0);
  EXPECT_TRUE(r["span"].is_array());
  EXPECT_FALSE(r["span"].empty());
  EXPECT_EQ(r.value("current_summary", "x"), "");  // first compaction: empty summary
  // While pending (no SESSION_SUMMARY/COMPACT_FAILED), further turns fire NO new request.
  const std::size_t before = reqs.size();
  bb.post("USER_MESSAGE", big + "again", "chat"); bb.pump();
  EXPECT_EQ(reqs.size(), before);
}

TEST(Arbiter, SessionSummaryAppliedFoldedAndCompactedPosted) {
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL"); a.on_attach(bb);
  nlohmann::json req, compacted;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.subscribe("COMPACTED",[&](const Entry& e){ compacted=e.value; });
  bb.post("USER_MESSAGE","one","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","a1"},{"epoch",1}}, "llm"); bb.pump();
  // session_path_ unset -> session_stem("") == "" matches the posted "".
  bb.post("SESSION_SUMMARY", {{"session",""},{"upto",2},{"text","WE DISCUSSED X"}}, "compactor");
  bb.pump();
  EXPECT_EQ(compacted.value("upto", 0), 2);
  bb.post("USER_MESSAGE","two","chat"); bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("Earlier in this session (compacted"), std::string::npos);
  EXPECT_NE(sys.find("WE DISCUSSED X"), std::string::npos);
  EXPECT_LT(sys.find("SOUL"), sys.find("Earlier in this session"));
}

TEST(Arbiter, SummaryGuardsRejectWrongSessionStaleAndOversizedUpto) {
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL"); a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","one","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","a1"},{"epoch",1}}, "llm"); bb.pump();
  bb.post("SESSION_SUMMARY", {{"session","other-session"},{"upto",2},{"text","WRONG"}}, "compactor");
  bb.post("SESSION_SUMMARY", {{"session",""},{"upto",99},{"text","OVERSIZED"}}, "compactor");
  bb.post("SESSION_SUMMARY", {{"session",""},{"upto",0},{"text","ZERO"}}, "compactor");
  bb.pump();
  bb.post("USER_MESSAGE","two","chat"); bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_EQ(sys, "SOUL");                          // none applied
}

TEST(Arbiter, CompactFailedRearmsDetection) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_history_budget_chars(400.0);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("COMPACT_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  const std::string big(150, 'x');
  for (int i = 0; i < 4; ++i) {
    bb.post("USER_MESSAGE", big, "chat"); bb.pump();
    bb.post("LLM_RESPONSE", {{"text","ok"},{"epoch",(std::uint64_t)(i+1)}}, "llm"); bb.pump();
  }
  ASSERT_GE(reqs.size(), 1u);
  const std::size_t before = reqs.size();
  bb.post("COMPACT_FAILED", {{"session",""},{"upto",reqs.back().value("upto",0)}}, "compactor");
  bb.pump();
  bb.post("USER_MESSAGE", big, "chat"); bb.pump(); // next turn re-fires
  EXPECT_GT(reqs.size(), before);
}

TEST(Arbiter, NewSessionResetsCompactionState) {
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL"); a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","one","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","a1"},{"epoch",1}}, "llm"); bb.pump();
  bb.post("SESSION_SUMMARY", {{"session",""},{"upto",2},{"text","OLD SESSION"}}, "compactor");
  bb.pump();
  bb.post("NEW_SESSION", true, "chat"); bb.pump();
  bb.post("USER_MESSAGE","fresh","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["content"].get<std::string>().find("OLD SESSION"),
            std::string::npos);
}

TEST(Arbiter, ResumeLoadsSidecarAndClampsCorruptPairing) {
  const std::string dir = ::testing::TempDir() + "/compact_resume_" + std::to_string(::getpid());
  std::filesystem::create_directories(dir);
  const std::string sp = dir + "/s1.jsonl";
  { std::ofstream f(sp);
    f << nlohmann::json{{"role","user"},{"content","q"}}.dump() << "\n"
      << nlohmann::json{{"role","assistant"},{"content","a"}}.dump() << "\n"; }
  { std::ofstream f(dir + "/s1.summary.md");
    f << serialize_summary_sidecar(2, "RESUMED SUMMARY"); }
  Blackboard bb; Arbiter a; a.set_system_prompt("SOUL");
  a.set_session_path(sp);
  a.on_attach(bb);
  a.load_history();
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_NE(req["messages"][0]["content"].get<std::string>().find("RESUMED SUMMARY"),
            std::string::npos);
  // Corrupt pairing: upto beyond history -> ignored entirely.
  const std::string sp2 = dir + "/s2.jsonl";
  { std::ofstream f(sp2); f << nlohmann::json{{"role","user"},{"content","q"}}.dump() << "\n"; }
  { std::ofstream f(dir + "/s2.summary.md"); f << serialize_summary_sidecar(50, "CORRUPT"); }
  Blackboard bb2; Arbiter a2; a2.set_system_prompt("SOUL");
  a2.set_session_path(sp2);
  a2.on_attach(bb2);
  a2.load_history();
  nlohmann::json req2;
  bb2.subscribe("LLM_REQUEST",[&](const Entry& e){ req2=e.value; });
  bb2.post("USER_MESSAGE","hi","chat"); bb2.pump();
  EXPECT_EQ(req2["messages"][0]["content"].get<std::string>(), "SOUL");
}

TEST(Arbiter, SidecarWrittenAtomicallyOnApply) {
  const std::string dir = ::testing::TempDir() + "/compact_write_" + std::to_string(::getpid());
  std::filesystem::create_directories(dir);
  const std::string sp = dir + "/w1.jsonl";
  Blackboard bb; Arbiter a; a.set_session_path(sp); a.on_attach(bb);
  bb.post("USER_MESSAGE","one","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","a1"},{"epoch",1}}, "llm"); bb.pump();
  bb.post("SESSION_SUMMARY", {{"session","w1"},{"upto",2},{"text","PERSISTED"}}, "compactor");
  bb.pump();
  std::ifstream f(dir + "/w1.summary.md");
  ASSERT_TRUE(f.good());
  std::stringstream ss; ss << f.rdbuf();
  const SidecarSummary s = parse_summary_sidecar(ss.str());
  EXPECT_EQ(s.upto, 2u);
  EXPECT_EQ(s.text, "PERSISTED");
  EXPECT_FALSE(std::filesystem::exists(dir + "/w1.summary.md.tmp"));
}
```

- [ ] **Step 2: Run — expect FAIL** (no COMPACT_REQUEST fired, no fold, guards absent).

- [ ] **Step 3: Implement.**

**(a)** `include/hades/arbiter.h` — add after the `turn_epoch_` member:

```cpp
  // Session compaction (opt-in Module = compactor; inert without it). summarized_upto_ =
  // leading history_ messages the rolling summary covers; pending_compact_ = one
  // COMPACT_REQUEST in flight (cleared by SESSION_SUMMARY or COMPACT_FAILED — the module's
  // worker ALWAYS posts a terminal reply, so this cannot wedge); summary_text_ is folded
  // into the leading system message (the sidecar file is persistence + human inspection,
  // NOT a live edit surface — the Arbiter is its single writer).
  std::size_t summarized_upto_ = 0;
  bool pending_compact_ = false;
  std::string summary_text_;
```

and two private method declarations next to `windowed_history_`:

```cpp
  // First history_ index the budget window includes (the budget walk + orphan adjustment
  // previously inline in windowed_history_). Everything before it is compaction's span.
  std::size_t window_start_() const;
  void on_session_summary(const Entry&);
```

**(b)** `src/apps/arbiter/arbiter.cpp` — add `#include "hades/compact/compact.h"`. Refactor `windowed_history_` into `window_start_` (same walk, returns `s`) plus a two-line suffix copy:

```cpp
std::size_t Arbiter::window_start_() const {
  const std::size_t n = history_.size();
  if (n == 0) return 0;
  std::size_t s = n;
  double total = 0.0;
  for (std::size_t i = n; i-- > 0;) {
    const double sz = static_cast<double>(
        history_[i].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size());
    if (s != n && total + sz > history_budget_chars_) break;
    total += sz;
    s = i;
  }
  while (s < n && history_[s].value("role", "") == "tool") ++s;
  if (s >= n) {
    s = n - 1;
    while (s > 0 && history_[s].value("role", "") == "tool") --s;
  }
  return s;
}

std::vector<nlohmann::json> Arbiter::windowed_history_() const {
  const std::size_t s = window_start_();
  return std::vector<nlohmann::json>(history_.begin() + static_cast<std::ptrdiff_t>(s),
                                     history_.end());
}
```

(Keep the original explanatory comment block on `window_start_`.)

**(c)** In `start_turn()`, replace the `for (const auto& m : windowed_history_())` line with a computed start + detection, placed just before the history append loop:

```cpp
  // Compaction detect: messages before the window start fall out of THIS request. Post at
  // most one in-flight COMPACT_REQUEST (the CompactorModule, if rostered, ALWAYS answers
  // with SESSION_SUMMARY or COMPACT_FAILED). Without the module nothing subscribes and the
  // single post is inert — today's behavior exactly.
  const std::size_t ws = window_start_();
  if (ws > summarized_upto_ && !pending_compact_) {
    std::vector<nlohmann::json> span(history_.begin() + static_cast<std::ptrdiff_t>(summarized_upto_),
                                     history_.begin() + static_cast<std::ptrdiff_t>(ws));
    bb_->post("COMPACT_REQUEST",
              {{"session", session_stem(session_path_)},
               {"upto", ws},
               {"span", digest_span(span)},
               {"current_summary", summary_text_}},
              "arbiter");
    pending_compact_ = true;
  }
  for (std::size_t i = ws; i < history_.size(); ++i) messages.push_back(history_[i]);
```

**(d)** The fold — in `start_turn()`, directly AFTER the core-memory fold block and BEFORE the SKILLS_ANNOUNCE fold:

```cpp
  // Rolling session summary (compaction): the agent's own earlier conversation, compacted.
  // Conversational context outranks tooling -> it sits right after core memory.
  if (!summary_text_.empty()) {
    if (!sys.empty()) sys += "\n\n";
    sys += "Earlier in this session (compacted from turns no longer shown; may be stale — "
           "re-verify files/live state before relying on a past action's result):\n" +
           summary_text_;
  }
```

**(e)** In `on_attach`, subscribe the two reply keys (next to the TOOL_RESULT subscription):

```cpp
  bb.subscribe("SESSION_SUMMARY", [this](const Entry& e) { on_session_summary(e); });
  bb.subscribe("COMPACT_FAILED", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    if (e.value.value("session", "") != session_stem(session_path_)) return;
    pending_compact_ = false;   // re-arm: next start_turn re-fires with the grown span
  });
```

**(f)** New method:

```cpp
// Apply a compactor reply. Guards: the session id must match the CURRENT session (a late
// worker from before a /new rotation must not contaminate the fresh session) and upto must
// advance monotonically within the loaded history. The sidecar write is atomic tmp+rename,
// best-effort (an IO failure loses persistence, never the in-memory summary).
void Arbiter::on_session_summary(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;
  if (v.value("session", "") != session_stem(session_path_)) return;
  pending_compact_ = false;
  const std::size_t upto =
      static_cast<std::size_t>(v.value("upto", static_cast<std::uint64_t>(0)));
  if (upto <= summarized_upto_ || upto > history_.size()) return;
  if (!v.contains("text") || !v["text"].is_string()) return;
  const std::string text = v["text"].get<std::string>();
  if (text.empty()) return;
  summary_text_ = text;
  summarized_upto_ = upto;
  const std::string sc = sidecar_path_for(session_path_);
  if (!sc.empty()) {
    const std::string tmp = sc + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (f) {
      f << serialize_summary_sidecar(summarized_upto_, summary_text_);
      f.close();
    }
    if (f) {
      std::error_code ec;
      std::filesystem::rename(tmp, sc, ec);
      if (ec) std::remove(tmp.c_str());
    } else {
      std::remove(tmp.c_str());
    }
  }
  bb_->post("COMPACTED",
            {{"upto", static_cast<std::uint64_t>(summarized_upto_)},
             {"chars", static_cast<std::uint64_t>(summary_text_.size())}},
            "arbiter");
}
```

(`#include <cstdio>` for `std::remove` if not already present.)

**(g)** NEW_SESSION handler — add to the existing subscriber body:

```cpp
    summary_text_.clear();
    summarized_upto_ = 0;
    pending_compact_ = false;
```

**(h)** `load_history()` — append at the end:

```cpp
  // Compaction sidecar: the same-id summary reloads with the session. Tolerant — missing/
  // garbage file or an upto beyond the loaded history (corrupt pairing) -> no summary; the
  // next window drop regenerates it.
  const std::string sc = sidecar_path_for(session_path_);
  if (!sc.empty()) {
    std::ifstream f(sc);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      const SidecarSummary s = parse_summary_sidecar(ss.str());
      if (!s.text.empty() && s.upto <= history_.size()) {
        summary_text_ = s.text;
        summarized_upto_ = s.upto;
      }
    }
  }
```

- [ ] **Step 4: Build + test.** `-R Arbiter` → all pass (7 new + every existing — the no-module lock is the existing suite passing unchanged). Full suite: 787/787.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/arbiter.h src/apps/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter compaction — window-drop detect, summary apply/fold, sidecar persist/resume"
```

---

## Task 3: CompactorModule

**Files:**
- Create: `include/hades/module/compactor_module.h`, `src/apps/compactor/compactor.cpp`
- Test: `tests/test_compactor_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `COMPACT_REQUEST` (T2), `Provider`/`LlmRequest` (`hades/llm/provider.h`), `Executor`, `trunc_utf8_bytes` (`hades/module/tool_runner.h`), `set_pos_double_on_string` (`hades/config.h`), `OpenAICompatProvider` + `cpr_http` (self-build path), `MalConfig`.
- Produces: `SESSION_SUMMARY {session, upto, text}` on success, `COMPACT_FAILED {session, upto}` on ANY failure (throw-wrapped, always-terminal), `AUX_SPENT_USD` delta. `CompactorModule(std::unique_ptr<Provider> p = nullptr)` injection ctor (test seam), `set_executor(Executor*)`.

- [ ] **Step 1: Write the failing tests** — create `tests/test_compactor_module.cpp`:

```cpp
// tests/test_compactor_module.cpp — CompactorModule: summarize-on-request, always-terminal
//
// FakeProvider + no executor -> the whole flow runs inline on pump(); the final test wires
// a REAL Arbiter and the module on one bus for the full detect->summarize->apply->fold loop.
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/module/compactor_module.h"
#include "hades/arbiter.h"
using namespace hades;

namespace {
struct FakeProvider : Provider {
  std::string reply = "MERGED SUMMARY";
  bool throw_on_call = false;
  std::vector<LlmRequest> seen;
  LlmResponse complete(const LlmRequest& r) override {
    seen.push_back(r);
    if (throw_on_call) throw std::runtime_error("aux down");
    LlmResponse resp;
    resp.text = reply;
    resp.prompt_tokens = 100;
    resp.completion_tokens = 50;
    return resp;
  }
};
// Construct-in-place rig: CompactorModule holds a std::atomic member, so it is neither
// copyable nor movable — the provider is injected at construction, never assigned after.
struct Rig {
  Blackboard bb;
  std::unique_ptr<FakeProvider> owned = std::make_unique<FakeProvider>();
  FakeProvider* prov = owned.get();
  CompactorModule m{std::move(owned)};
  void start(const Block& cfg = Block{}) { m.on_start(cfg, bb); m.on_attach(bb); }
};
nlohmann::json request(int upto = 3) {
  return {{"session", "s1"},
          {"upto", upto},
          {"span", nlohmann::json::array({{{"role", "user"}, {"content", "we set port 9090"}}})},
          {"current_summary", "PRIOR SUMMARY"}};
}
}  // namespace

TEST(CompactorModule, SummarizesAndPostsSessionSummary) {
  Rig r;
  r.start();
  nlohmann::json out, aux;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  ASSERT_FALSE(out.is_null());
  EXPECT_EQ(out.value("session", ""), "s1");
  EXPECT_EQ(out.value("upto", 0), 3);
  EXPECT_EQ(out.value("text", ""), "MERGED SUMMARY");
  // The request carried BOTH the prior summary and the span content (rolling merge input).
  ASSERT_EQ(r.prov->seen.size(), 1u);
  const std::string user = r.prov->seen[0].messages.back()["content"].get<std::string>();
  EXPECT_NE(user.find("PRIOR SUMMARY"), std::string::npos);
  EXPECT_NE(user.find("we set port 9090"), std::string::npos);
  EXPECT_TRUE(aux.is_null());   // price_per_mtok unset -> no spend delta
}

TEST(CompactorModule, ProviderThrowPostsCompactFailed) {
  Rig r;
  r.prov->throw_on_call = true;
  r.start();
  nlohmann::json failed, out;
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry& e) { failed = e.value; });
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  EXPECT_TRUE(out.is_null());
  ASSERT_FALSE(failed.is_null());                  // ALWAYS-terminal: failure still replies
  EXPECT_EQ(failed.value("session", ""), "s1");
  EXPECT_EQ(failed.value("upto", 0), 3);
}

TEST(CompactorModule, ReplyTruncatedToCharLimit) {
  Rig r;
  r.prov->reply = std::string(200, 's');
  Block cfg; cfg.kv["summary_char_limit"] = "50";
  r.start(cfg);
  nlohmann::json out;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  const std::string text = out.value("text", "");
  EXPECT_LE(text.size(), 50u + 12u);               // limit + " (truncated)" marker
  EXPECT_NE(text.find(" (truncated)"), std::string::npos);
}

TEST(CompactorModule, MalformedAndEmptyRequestsIgnored) {
  Rig r;
  r.start();
  bool any = false;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry&) { any = true; });
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry&) { any = true; });
  r.bb.post("COMPACT_REQUEST", "not an object", "x");
  r.bb.post("COMPACT_REQUEST", {{"session", "s"}, {"upto", 0},
                                {"span", nlohmann::json::array()}}, "x");  // empty span / upto 0
  r.bb.pump();
  EXPECT_FALSE(any);                               // not from the Arbiter's detect -> no reply owed
}

TEST(CompactorModule, PriceYieldsAuxSpendDelta) {
  Rig r;
  Block cfg; cfg.kv["price_per_mtok"] = "10.0";
  r.start(cfg);
  nlohmann::json aux;
  r.bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  ASSERT_TRUE(aux.is_number());
  EXPECT_NEAR(aux.get<double>(), 150.0 / 1e6 * 10.0, 1e-12);
}

TEST(CompactorModule, FullLoopDetectSummarizeApplyFold) {
  Rig r;                                           // FakeProvider replies "MERGED SUMMARY"
  Arbiter a; a.set_system_prompt("SOUL");
  a.set_history_budget_chars(400.0);
  a.on_attach(r.bb);
  r.start();
  nlohmann::json req;
  r.bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  const std::string big(150, 'x');
  for (int i = 0; i < 5; ++i) {                    // overflow -> detect -> summarize -> apply
    r.bb.post("USER_MESSAGE", big + std::to_string(i), "chat"); r.bb.pump();
    r.bb.post("LLM_RESPONSE", {{"text", "ok"}, {"epoch", (std::uint64_t)(i + 1)}}, "llm");
    r.bb.pump();
  }
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("Earlier in this session (compacted"), std::string::npos);
  EXPECT_NE(sys.find("MERGED SUMMARY"), std::string::npos);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add `target_sources(hades_core PRIVATE src/apps/compactor/compactor.cpp)` and `target_sources(hades_tests PRIVATE tests/test_compactor_module.cpp)`. Compile fails (no header).

- [ ] **Step 3: Implement.** `include/hades/module/compactor_module.h`:

```cpp
// include/hades/module/compactor_module.h — background session summarizer (compaction)
//
// Answers the Arbiter's COMPACT_REQUEST with an aux LLM call that MERGES the existing
// rolling summary with the newly dropped turns (auto-extract discipline: own provider from
// the merged cfg, Executor worker, one in flight, AUX_SPENT_USD delta). The worker is
// throw-wrapped and ALWAYS posts a terminal reply — SESSION_SUMMARY on success,
// COMPACT_FAILED on any error — so the Arbiter's pending flag can never wedge (the
// tool-offload BG_DONE lesson). Fail-soft: no module in the roster -> no subscriber ->
// silent-truncation behavior unchanged.
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class Executor;

class CompactorModule : public Module {
public:
  explicit CompactorModule(std::unique_ptr<Provider> p = nullptr) : provider_(std::move(p)) {}
  std::string type() const override { return "compactor"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* e) { executor_ = e; }

private:
  std::unique_ptr<Provider> provider_;   // injected (tests) or built in on_start
  Executor* executor_ = nullptr;         // nullptr -> summarize runs inline (tests)
  Blackboard* bb_ = nullptr;
  std::string model_;
  double price_per_mtok_ = 0.0;
  std::size_t summary_char_limit_ = 4000;
  std::atomic<bool> busy_{false};        // one summarize in flight; worker clears it
};
}  // namespace hades
```

`src/apps/compactor/compactor.cpp`:

```cpp
// src/apps/compactor/compactor.cpp — CompactorModule (see the header)
#include "hades/module/compactor_module.h"
#include <cstdlib>
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/launcher.h"              // MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/module/tool_runner.h"    // trunc_utf8_bytes
namespace hades {
namespace {
constexpr const char* kSystemPrompt =
    "You maintain a rolling summary of an ongoing conversation between a user and their AI "
    "assistant. Merge the existing summary with the newly dropped turns into ONE updated "
    "summary. Keep: decisions made, open tasks, user preferences and corrections, key facts, "
    "and file/tool state. Drop: pleasantries, superseded attempts, and tool noise. Reply with "
    "ONLY the updated summary text.";

// The merge input: prior summary + the span digest, flattened to plain text.
std::string build_merge_input(const std::string& current, const nlohmann::json& span,
                              std::size_t char_limit) {
  std::string in = "Existing summary (may be empty):\n" + current + "\n\nNewly dropped turns:\n";
  for (const auto& m : span) {
    if (!m.is_object()) continue;
    in += m.value("role", "") + ": " + m.value("content", "") + "\n";
  }
  in += "\nUpdated summary (under " + std::to_string(char_limit) + " characters):";
  return in;
}
}  // namespace

void CompactorModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("model")) model_ = cfg.kv.at("model");
  if (cfg.kv.count("price_per_mtok"))
    set_pos_double_on_string(cfg.kv.at("price_per_mtok"), price_per_mtok_);
  if (cfg.kv.count("summary_char_limit")) {
    try {
      const long n = std::stol(cfg.kv.at("summary_char_limit"));
      if (n > 0) summary_char_limit_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default */ }
  }
  if (provider_) return;  // injected (tests)
  double timeout_s = 60.0;
  if (cfg.kv.count("timeout_s")) set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s);
  const std::string ep  = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
  const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("compactor: api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model_, cpr_http(timeout_s));
}

void CompactorModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("COMPACT_REQUEST", [this](const Entry& e) {
    // Gate on the pump thread; the summarize itself runs on a worker (inline w/o executor).
    const auto& v = e.value;
    if (!v.is_object()) return;
    const std::string session = v.value("session", "");
    const std::size_t upto =
        static_cast<std::size_t>(v.value("upto", static_cast<std::uint64_t>(0)));
    if (upto == 0) return;
    if (!v.contains("span") || !v["span"].is_array() || v["span"].empty()) return;
    if (busy_.exchange(true)) return;   // one in flight (the Arbiter never double-sends)
    // Capture discipline (LLMModule/auto-extract precedent): non-owning provider/bus
    // pointers + plain values + the atomic busy flag. No pump-mutated field off-thread.
    Provider* prov = provider_.get();
    Blackboard* bus = bb_;
    std::atomic<bool>* busy = &busy_;
    const double price = price_per_mtok_;
    const std::size_t limit = summary_char_limit_;
    LlmRequest req;
    req.model = model_;
    req.messages = {
        nlohmann::json{{"role", "system"}, {"content", kSystemPrompt}},
        nlohmann::json{{"role", "user"},
                       {"content", build_merge_input(v.value("current_summary", ""),
                                                     v["span"], limit)}}};
    // ALWAYS-terminal worker (tool-offload BG_DONE lesson): success -> SESSION_SUMMARY,
    // any throw/empty reply -> COMPACT_FAILED. The Arbiter's pending flag depends on it.
    auto run = [prov, bus, busy, session, upto, limit, price](const LlmRequest& r) {
      bool ok = false;
      std::string text;
      try {
        const LlmResponse resp = prov->complete(r);
        text = trunc_utf8_bytes(resp.text, limit);
        ok = !text.empty();
        const double delta =
            (static_cast<double>(resp.prompt_tokens) + resp.completion_tokens) / 1e6 * price;
        if (delta > 0.0) bus->post("AUX_SPENT_USD", delta, "compactor");
      } catch (...) {
        ok = false;
      }
      if (ok)
        bus->post("SESSION_SUMMARY",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)},
                   {"text", text}},
                  "compactor");
      else
        bus->post("COMPACT_FAILED",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)}},
                  "compactor");
      busy->store(false);
    };
    if (executor_) {
      // Enqueue-throw would leak busy_=true AND leave the Arbiter pending — post the
      // terminal failure inline (we are on the pump thread; post() is safe).
      try {
        executor_->submit([req = std::move(req), run] { run(req); });
      } catch (...) {
        busy_.store(false);
        bb_->post("COMPACT_FAILED",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)}},
                  "compactor");
      }
    } else {
      run(req);
    }
  });
}
}  // namespace hades
```


- [ ] **Step 4: Build + test.** `-R CompactorModule` → 6 pass; full suite 793/793.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/compactor_module.h src/apps/compactor/compactor.cpp tests/test_compactor_module.cpp CMakeLists.txt
git commit -m "feat: CompactorModule — rolling-merge aux summarizer, always-terminal reply"
```

---

## Task 4: Wiring — factory, `Compactor` merged cfg, member order

**Files:**
- Modify: `app/agent_wiring.h` (Agent member + include), `app/agent_wiring.cpp` (factory, take_as, wire_agent param + 2g block, Manifest overload block extract + call)
- Create: `tests/test_compactor_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CompactorModule` (T3).
- Produces: `Agent::compactor` (`std::unique_ptr<CompactorModule>`, declared **directly after `auto_extract` and BEFORE `executor`** — the executor must join the worker while the module is alive); roster factory `"compactor"`; `Compactor` manifest block merged with Session transport values (the AutoExtract 2f pattern); test overload leaves it null.

- [ ] **Step 1: Write the failing tests** — create `tests/test_compactor_wiring.cpp`:

```cpp
// tests/test_compactor_wiring.cpp — manifest-path wiring for Module = compactor
#include <gtest/gtest.h>
#include <cstdlib>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(CompactorWiring, RosteredModuleIsBuiltAndAbsentIsNull) {
  ::setenv("HADES_API_KEY", "dummy", 1);  // on_start builds a real provider (no call made)
  Blackboard bb;
  Manifest with = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = compactor\nModule = arbiter\n"
      "Compactor\n{\n  model = cheap-model\n  summary_char_limit = 2000\n}\n");
  Agent a = build_agent(bb, with);
  EXPECT_NE(a.compactor, nullptr);
  Blackboard bb2;
  Manifest without = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent b = build_agent(bb2, without);
  EXPECT_EQ(b.compactor, nullptr);
}

TEST(CompactorWiring, MissingKeyEnvFailsLoud) {
  ::unsetenv("HADES_COMPACT_KEY_MISSING");
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = compactor\nModule = arbiter\n"
      "Compactor\n{\n  api_key_env = HADES_COMPACT_KEY_MISSING\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add `target_sources(hades_tests PRIVATE tests/test_compactor_wiring.cpp)`. Compile fails: `Agent` has no member `compactor`.

- [ ] **Step 3: Implement.** In `app/agent_wiring.h`: add `#include "hades/module/compactor_module.h"` with the module includes and, in `struct Agent`, **directly after the `auto_extract` member**:

```cpp
  std::unique_ptr<CompactorModule> compactor;  // optional session summarizer (compaction)
```

(`executor` and the thread-owning tail stay where they are — do NOT reorder anything else.)

In `app/agent_wiring.cpp`:

1. `wire_agent` signature gains a trailing defaulted param `const Block& compactor_cfg = Block{}` (after `tools_cfg`).
2. Directly AFTER the 2f auto-extract block, add:

```cpp
  // 2g) CompactorModule: merged cfg = Session transport values + Compactor overrides (the
  //     2f auto-extract pattern; aux default model = the main model). Executor set BEFORE
  //     on_attach; the Executor joins the worker before this module dies (member order).
  if (a.compactor) {
    Block cmerged = compactor_cfg;                 // model / summary_char_limit / timeout_s
    auto cinherit = [&](const char* k) {
      if (!cmerged.kv.count(k) && session.kv.count(k)) cmerged.kv[k] = session.kv.at(k);
    };
    cinherit("endpoint");
    cinherit("api_key_env");
    cinherit("price_per_mtok");
    if (!cmerged.kv.count("model") && session.kv.count("model"))
      cmerged.kv["model"] = session.kv.at("model");
    if (a.executor) a.compactor->set_executor(a.executor.get());
    a.compactor->on_start(cmerged, bb);
    a.compactor->on_attach(bb);
  }
```

3. Manifest overload: register the factory with the others
   (`launcher.register_factory("compactor", []{ return std::make_unique<CompactorModule>(); });`),
   take it (`a.compactor = take_as<CompactorModule>(launcher, "compactor");`), extract the block
   next to the AutoExtract one:

```cpp
  const auto compactor_blocks = m.of("Compactor");
  const Block compactor_cfg = compactor_blocks.empty() ? Block{} : compactor_blocks.front();
```

   and append `compactor_cfg` as the new last argument of the `wire_agent(...)` call.

- [ ] **Step 4: Build + test.** `-R CompactorWiring` → 2 pass; FULL suite 795/795 (test overload passes `Block{}` by default → untouched).
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp tests/test_compactor_wiring.cpp CMakeLists.txt
git commit -m "feat: wire compactor — Agent member, roster factory, Compactor merged cfg"
```

---

## Task 5: Ship — docs, dev.hades, soul.md, CLAUDE.md, both lanes

**Files:**
- Modify: `manifests/dev.hades`, `prompts/soul.md`, `docs/manifest-reference.md`, `CLAUDE.md`

- [ ] **Step 1: dev.hades** — add near the AutoExtract block, COMMENTED (public template default = today's behavior):

```
# --- Session compaction (opt-in): turns dropped from the request window get summarized
# into .hades/sessions/<id>.summary.md and folded back into context. Uses the Session
# provider unless overridden here.
# Module = compactor
# Compactor
# {
#   model              = <aux model>   # default: Session.model
#   summary_char_limit = 4000
#   timeout_s          = 60
# }
```

- [ ] **Step 2: soul.md** — append one paragraph to the `## Memory` section:

```markdown
In long sessions your oldest turns are compacted: the "Earlier in this session" block is
your own earlier conversation, summarized. Treat it as yours — but like session excerpts,
re-verify files and live state before relying on a past action's result.
```

- [ ] **Step 3: manifest-reference.md** — add the next numbered section, "`Compactor` block — session compaction (optional)": the module line + block keys table (`model` default Session.model / `summary_char_limit` 4000 / `timeout_s` 60 / inherited `endpoint`/`api_key_env`/`price_per_mtok`), behavior paragraph (window-drop → background aux merge → sidecar `.hades/sessions/<id>.summary.md` → system-prompt fold; fail-soft: without the module, silent truncation as before; spend metered via AUX_SPENT_USD), the new bus keys, and cross-references: §2 roster row (`compactor`), Session-table `history_budget_chars` row gains "see the Compactor section — compaction summarizes what falls outside this window". Verify section numbering against the file's current state.

- [ ] **Step 4: CLAUDE.md** — new `### Session compaction` subsection under Current state (shape, decisions, guards, sidecar, v1 edges: summary lags one turn behind a drop; window-overlap duplication benign; compaction runs for all origins), strike the "Context-full behavior — DECIDE" backlog item as DECIDED+SHIPPED, update the header test count, add `compactor` to the roster list in the §2-adjacent prose if mentioned.

- [ ] **Step 5: Full verification, BOTH lanes.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan --output-on-failure
```

Expected: ALL green in both (795/795 — verify the actual count and use IT in the docs; brief predictions have drifted twice before).

- [ ] **Step 6: Commit.**

```bash
git add manifests/dev.hades prompts/soul.md docs/manifest-reference.md CLAUDE.md
git commit -m "feat: ship compactor — dev.hades, soul.md, manifest-reference, CLAUDE.md"
```

---

## Verification (end-to-end)

1. Full suite both lanes: expected 795/795 (774 baseline + 6 T1 + 7 T2 + 6 T3 + 2 T4).
2. Manual live smoke (Vaios, dev.local.hades + `Module = compactor` + block):
   ```
   # temporarily set history_budget_chars = 4000 to force overflow fast
   # have a 15+ turn conversation; then:
   cat .hades/sessions/<id>.summary.md      # sidecar exists, upto grows
   # ask "what did we discuss at the start?" -> answered from the summary
   # restart with --resume -> summary still folded
   ```
3. `hades-scope session.log COMPACT` shows request/summary/COMPACTED flow.

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus), per-task cpp-reviewer (opus), final whole-branch review, then finishing-a-development-branch (merge ff to main — push ONLY on Vaios's word).
