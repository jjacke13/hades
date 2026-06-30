# GET /history web re-render Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Implementers run on OPUS.** Read the named files fully before editing. **DISCIPLINE:** build + FULL `ctest` suite + `git commit` + verify `git log` shows your commit + write report BEFORE replying DONE.

**Goal:** `hades <manifest> --serve --resume` renders the resumed conversation in the browser instead of starting blank, via a CSRF-gated `GET /history` JSON endpoint that reads the per-session jsonl from disk + a fetch-on-load render in the web UI (including dim tool turns).

**Architecture:** A pure `read_session_jsonl(path)` tolerantly parses the persisted `.hades/sessions/<id>.jsonl`. `HttpServerModule` gains a `set_session_path` + a socket-free `history_json()` that wraps it, exposed at `GET /history`; the existing CSRF chokepoint is promoted to a testable `static HttpServerModule::authorize` and extended to gate `/history`. `web/app.js` fetches `/history` on load and renders user/assistant bubbles plus dim tool-call/tool-result entries. The endpoint reads disk only — no Arbiter coupling, no lock.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell (`nix develop`) · cpp-httplib · nlohmann/json · GoogleTest · plain-JS static web UI (no framework, no JS test harness).

Spec: `docs/superpowers/specs/2026-06-30-get-history-web-render-design.md` (read first).

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`** (e.g. `nix develop --command ctest --test-dir build`).
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer / no Co-Authored-By**.
- **Backward-compat:** the existing **197** tests stay green. A fresh (non-resume) launch returns `{"history": []}` → blank page, identical to today.
- **Immutability:** `read_session_jsonl` returns a fresh `std::vector`; `history_json()` builds a fresh object. No in-place mutation of shared state.
- **Security:** `GET /history` is CSRF-gated with the `X-Hades` header (cross-origin can't add it without a preflight we never grant); static `GET /` stays exempt so the UI loads. The endpoint reads only the gitignored `.hades/` session file (conversation, never the api key).
- **No orphan-strip in the display reader:** `read_session_jsonl` returns messages AS-IS (a trailing `assistant(tool_calls)` survives) — unlike `Arbiter::load_history`, whose strip is for provider-request validity only.
- **Threading:** the `GET /history` handler runs on the httplib thread and only reads a file — no shared mutable state, **no `mu_`**. A concurrent Arbiter append that leaves a half-written final line is skipped by the tolerant parse.
- **Web UI** stays plain JS (no framework); changes verified by inspection + a manual `--serve --resume` smoke (no JS test harness exists).

## File Structure

```
include/hades/session_history.h        T1 (new)     declares read_session_jsonl
src/core/session_history.cpp           T1 (new)     tolerant jsonl reader
tests/test_session_history.cpp         T1 (new)     unit tests for the reader
src/arbiter/arbiter.cpp                T1 (modify)  load_history reuses read_session_jsonl (dedup)
CMakeLists.txt                         T1 (modify)  register the new core source + test

include/hades/module/http_server_module.h  T2 (modify)  set_session_path, history_json, static authorize, session_path_ member, httplib::Request fwd-decl
src/module/http_server_module.cpp           T2 (modify)  promote authorize (gate /history), history_json(), GET /history route
app/hades_main.cpp                          T2 (modify)  serve->set_session_path(session_path)
tests/test_serve.cpp                        T2 (extend)  history_json + authorize tests

web/app.js                             T3 (modify)  fetch-on-load + addToolCall/addToolResult/renderHistory
web/style.css                          T3 (modify)  dim .tool-call / .tool-result styling
```

---

## Task 1: `read_session_jsonl` — pure tolerant reader (+ dedup `load_history`)

**Files:** Create `include/hades/session_history.h`, `src/core/session_history.cpp`, `tests/test_session_history.cpp`. Modify `src/arbiter/arbiter.cpp` (only the `load_history` parse loop), `CMakeLists.txt`. **Read first:** `src/arbiter/arbiter.cpp:41-66` (`load_history`), `CMakeLists.txt:18-37`.

**Interfaces — Produces:** `std::vector<nlohmann::json> read_session_jsonl(const std::string& path)` — tolerant (skip blank lines + any non-object/`is_discarded` line, incl. a partial trailing line); empty path / missing file → empty vector; **no orphan-strip**.

- [ ] **Step 1: Failing tests** — create `tests/test_session_history.cpp`:

```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "hades/session_history.h"
using namespace hades;

static std::string write_tmp(const std::string& name, const std::string& body) {
  std::string p = testing::TempDir() + "/" + name;
  std::ofstream f(p);
  f << body;
  return p;
}

TEST(SessionHistory, RoundTripsMessages) {
  std::string p = write_tmp("sh_ok.jsonl",
    "{\"role\":\"user\",\"content\":\"hi\"}\n"
    "{\"role\":\"assistant\",\"content\":\"hello\"}\n");
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].value("role", ""), "user");
  EXPECT_EQ(v[0].value("content", ""), "hi");
  EXPECT_EQ(v[1].value("content", ""), "hello");
}

TEST(SessionHistory, MissingFileAndEmptyPathYieldEmpty) {
  EXPECT_TRUE(read_session_jsonl("").empty());
  EXPECT_TRUE(read_session_jsonl(testing::TempDir() + "/sh_does_not_exist.jsonl").empty());
}

TEST(SessionHistory, SkipsBlankCorruptAndPartialTrailing) {
  std::string p = write_tmp("sh_dirty.jsonl",
    "{\"role\":\"user\",\"content\":\"a\"}\n"
    "\n"                                        // blank line
    "not json at all\n"                         // corrupt interior
    "[1,2,3]\n"                                 // valid JSON but not an object
    "{\"role\":\"assistant\",\"content\":\"b\"}\n"
    "{\"role\":\"user\",\"content\":\"trunc");  // partial trailing (unterminated, no newline)
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].value("content", ""), "a");
  EXPECT_EQ(v[1].value("content", ""), "b");
}

TEST(SessionHistory, KeepsTrailingToolCallOrphan) {
  // Display reader keeps everything parseable (unlike Arbiter::load_history, which strips
  // boundary orphans for provider validity): a dangling assistant(tool_calls) survives.
  std::string p = write_tmp("sh_orphan.jsonl",
    "{\"role\":\"user\",\"content\":\"go\"}\n"
    "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"
    "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"fs_read\",\"arguments\":\"{}\"}}]}\n");
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_TRUE(v[1].contains("tool_calls"));
}
```

- [ ] **Step 2: Register + run, expect FAIL.** Add to `CMakeLists.txt` (next to the other core sources, after line 21 `src/core/session_id.cpp`):

```cmake
target_sources(hades_core PRIVATE src/core/session_history.cpp)
```

and next to the other test sources (after line 37 `tests/test_session_id.cpp`):

```cmake
target_sources(hades_tests PRIVATE tests/test_session_history.cpp)
```

Run: `nix develop --command cmake --build build` → expect a compile/link error (no `read_session_jsonl`, no `session_history.cpp`).

- [ ] **Step 3: Implement.** Create `include/hades/session_history.h`:

```cpp
// include/hades/session_history.h — tolerant reader for a per-session conversation jsonl.
//
// Pure: reads one JSON message per line from `path`, skipping blank lines and any line that does
// not parse to a JSON object (a corrupt interior line, or a partially-written trailing line from a
// concurrent append). Returns the raw stored messages AS-IS — NO orphan-pair sanitize (this feeds
// the browser GET /history for display, not a provider request). Empty path / missing file -> [].
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {
std::vector<nlohmann::json> read_session_jsonl(const std::string& path);
}  // namespace hades
```

Create `src/core/session_history.cpp`:

```cpp
#include "hades/session_history.h"
#include <fstream>
#include <string>
#include <utility>
namespace hades {
std::vector<nlohmann::json> read_session_jsonl(const std::string& path) {
  std::vector<nlohmann::json> out;
  if (path.empty()) return out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh/absent session, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);  // false = no throw, returns discarded
    if (!j.is_discarded() && j.is_object()) out.push_back(std::move(j));
  }
  return out;
}
}  // namespace hades
```

- [ ] **Step 4: Dedup `Arbiter::load_history`.** In `src/arbiter/arbiter.cpp`, add the include near the existing ones (after line 12 `#include "hades/session_id.h"`):

```cpp
#include "hades/session_history.h"   // read_session_jsonl (shared tolerant parse)
```

Replace the parse loop in `load_history` (currently lines 47-55, from `std::ifstream f(session_path_);` through the `while (std::getline(...))` block) so the function becomes exactly:

```cpp
void Arbiter::load_history() {
  if (session_path_.empty()) return;
  // Tolerant parse shared with the GET /history display reader (skip blank/corrupt/partial lines).
  auto loaded = read_session_jsonl(session_path_);
  history_.insert(history_.end(), loaded.begin(), loaded.end());
  // Drop leading orphan tool message(s); the window must begin on user/assistant (or be empty).
  while (!history_.empty() && history_.front().value("role", "") == "tool")
    history_.erase(history_.begin());
  // Mid-pair crash: a truncated/lost tool-result line can leave a trailing assistant(tool_calls)
  // (an assistant tool-call with no following tool result). The next user turn would form
  // [assistant(tool_calls), user], which providers reject — drop the trailing orphan.
  while (!history_.empty() &&
         history_.back().value("role", "") == "assistant" &&
         history_.back().contains("tool_calls"))
    history_.pop_back();
}
```

Keep the explanatory comment block above the function (lines 41-45) unchanged. The `#include <fstream>` in arbiter.cpp may now be unused by `load_history` — leave it (other code, e.g. `append_history`, still uses `<fstream>`; do not remove includes unless the build warns and you confirm no other use).

- [ ] **Step 5: Build + test.** Run:
```
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure -R "SessionHistory|Arbiter"
```
Expected: the 4 `SessionHistory` tests PASS and all existing `Arbiter` tests still PASS (the refactor preserves behavior: same skip rules, same orphan-strip). Then the FULL suite:
```
nix develop --command ctest --test-dir build
```
Expected: 197 + 4 = **201** tests pass.

- [ ] **Step 6: Commit.**
```
git add include/hades/session_history.h src/core/session_history.cpp tests/test_session_history.cpp src/arbiter/arbiter.cpp CMakeLists.txt
git commit -m "feat: read_session_jsonl tolerant reader (+ dedup Arbiter::load_history)"
```

---

## Task 2: `GET /history` endpoint + CSRF gate + wiring

**Files:** Modify `include/hades/module/http_server_module.h`, `src/module/http_server_module.cpp`, `app/hades_main.cpp`. Extend `tests/test_serve.cpp`. **Read first:** all of `src/module/http_server_module.cpp` (esp. the anonymous-namespace `authorize` at lines 21-25, `listen()` at 120-162), `include/hades/module/http_server_module.h`, `tests/test_serve.cpp` (the `ProbeServer` pattern + the existing tests), `app/hades_main.cpp:116-141` (where `session_path` is wired to the Arbiter).

**Interfaces — Consumes:** `read_session_jsonl` (Task 1). **Produces:** `void HttpServerModule::set_session_path(std::string)`, `nlohmann::json HttpServerModule::history_json() const` (returns `{"history": [...]}`), `static bool HttpServerModule::authorize(const httplib::Request&)`; `GET /history` route.

- [ ] **Step 1: Failing tests** — append to `tests/test_serve.cpp` (after the last test). The file already `#include <httplib.h>`, `<fstream>` is NOT included — add `#include <fstream>` to the include block at the top:

```cpp
TEST(HttpServer, HistoryJsonReadsSessionFile) {
  std::string p = testing::TempDir() + "/serve_hist.jsonl";
  {
    std::ofstream f(p);
    f << "{\"role\":\"user\",\"content\":\"hi\"}\n"
      << "{\"role\":\"assistant\",\"content\":\"yo\"}\n";
  }
  HttpServerModule srv;
  srv.set_session_path(p);
  auto out = srv.history_json();
  ASSERT_TRUE(out.contains("history"));
  ASSERT_EQ(out["history"].size(), 2u);
  EXPECT_EQ(out["history"][0].value("content", ""), "hi");
  EXPECT_EQ(out["history"][1].value("content", ""), "yo");
}

TEST(HttpServer, HistoryJsonEmptyWhenNoSessionPath) {
  HttpServerModule srv;  // no session path set
  auto out = srv.history_json();
  ASSERT_TRUE(out.contains("history"));
  EXPECT_TRUE(out["history"].empty());
}

TEST(HttpServer, AuthorizeGatesHistoryAndPostsButExemptsStaticGet) {
  httplib::Request get_hist;
  get_hist.method = "GET";
  get_hist.path = "/history";
  EXPECT_FALSE(HttpServerModule::authorize(get_hist));  // GET /history without X-Hades -> blocked
  get_hist.set_header("X-Hades", "1");
  EXPECT_TRUE(HttpServerModule::authorize(get_hist));    // with header -> allowed

  httplib::Request get_root;
  get_root.method = "GET";
  get_root.path = "/";
  EXPECT_TRUE(HttpServerModule::authorize(get_root));     // static GET exempt (UI must load)

  httplib::Request post_chat;
  post_chat.method = "POST";
  post_chat.path = "/chat";
  EXPECT_FALSE(HttpServerModule::authorize(post_chat));   // POST /chat without X-Hades -> blocked
  post_chat.set_header("X-Hades", "1");
  EXPECT_TRUE(HttpServerModule::authorize(post_chat));
}
```

- [ ] **Step 2: Run, expect FAIL** (`set_session_path`/`history_json`/`authorize` not members yet): `nix develop --command cmake --build build` → compile error.

- [ ] **Step 3: Header changes** — edit `include/hades/module/http_server_module.h`:

In the `namespace httplib { class Server; }` forward-decl block, add the `Request` struct:

```cpp
namespace httplib {
class Server;
struct Request;
}
```

In the `public:` section (e.g. right after the `idle_timeout_s()` getter, before `configure_server_`), add:

```cpp
  // The --serve front-end reads the same per-session conversation jsonl that the Arbiter persists,
  // so GET /history can re-render a resumed transcript. Wiring sets this to the resolved session
  // path (empty -> history_json() returns {"history":[]}). Read-only; no coupling to the Arbiter.
  void set_session_path(std::string p) { session_path_ = std::move(p); }
  // Socket-free body of GET /history: {"history": [ ...raw stored messages... ]} read from disk.
  // Const + socket-free so a test can assert it without binding a socket (mirrors handle_message).
  nlohmann::json history_json() const;

  // CSRF chokepoint (was a file-local helper; promoted to a public static so a test can assert it
  // without a socket, like configure_server_). Returns false for a request that must be blocked:
  // a tool-invoking POST (/chat,/confirm) or GET /history lacking the X-Hades header that a
  // cross-origin "simple" request cannot add without a preflight we never grant. Static GETs pass.
  static bool authorize(const httplib::Request& req);
```

The header already includes `<utility>` transitively? It does NOT — `std::move` needs `<utility>`. Add `#include <utility>` to the header's include block (next to `<mutex>`/`<string>`).

In the `private:` section, add the member (next to `pending_confirm_`):

```cpp
  std::string session_path_;  // per-session jsonl read by GET /history (set by wiring; may be empty)
```

- [ ] **Step 4: Implementation** — edit `src/module/http_server_module.cpp`:

Add the include near the top (after `#include "hades/timeouts.h"`):

```cpp
#include "hades/session_history.h"  // read_session_jsonl (GET /history)
```

**Delete** the anonymous-namespace `authorize` free function (lines 21-25, the `bool authorize(const httplib::Request& req) { ... }` — keep the explanatory comment above it if you like, but the function body moves to a member). Then **define** the static member (place it next to `configure_server_`'s definition, inside `namespace hades`):

```cpp
bool HttpServerModule::authorize(const httplib::Request& req) {
  // Tool-invoking POSTs and the conversation-returning GET /history require the custom header a
  // cross-origin simple request cannot add without a preflight we never grant. Static GETs (the
  // UI itself) stay exempt so the page can load.
  if (req.method == "POST" && (req.path == "/chat" || req.path == "/confirm"))
    return req.has_header("X-Hades");
  if (req.path == "/history")
    return req.has_header("X-Hades");
  return true;
}
```

Add the `history_json()` definition (e.g. after `collect_()` or near `handle_message`):

```cpp
nlohmann::json HttpServerModule::history_json() const {
  // Disk read only; no shared mutable state -> no mu_. A concurrent Arbiter append that leaves a
  // half-written final line is skipped by read_session_jsonl's tolerant parse.
  return {{"history", read_session_jsonl(session_path_)}};
}
```

In `listen()`, update the pre-routing handler to call the static member (it currently calls the now-deleted free `authorize`): change `if (!authorize(req))` to `if (!HttpServerModule::authorize(req))`. Then add the route (next to the `GET /health` registration):

```cpp
  srv.Get("/history", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(history_json().dump(), "application/json");
  });
```

- [ ] **Step 5: Wiring** — edit `app/hades_main.cpp`. After the Arbiter session wiring (`agent.arbiter->set_session_dir(sessions_dir);`, line 131), add:

```cpp
    // The --serve front-end reads the same session jsonl for GET /history (resumed-transcript
    // render). Null-guarded: a REPL-only roster omits `serve`. Same resolved path as the Arbiter.
    if (agent.serve) agent.serve->set_session_path(session_path);
```

- [ ] **Step 6: Build + test.** Run:
```
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure -R "HttpServer|Serve"
```
Expected: the 3 new `HttpServer` tests PASS plus the existing serve tests. Then the FULL suite:
```
nix develop --command ctest --test-dir build
```
Expected: **204** tests pass (201 + 3).

- [ ] **Step 7: Commit.**
```
git add include/hades/module/http_server_module.h src/module/http_server_module.cpp app/hades_main.cpp tests/test_serve.cpp
git commit -m "feat: GET /history endpoint (disk-read, CSRF-gated) + serve session-path wiring"
```

---

## Task 3: Web UI — fetch `/history` on load + render tool turns

**Files:** Modify `web/app.js`, `web/style.css`. **No automated test** (static plain-JS UI, no harness) — verified by inspection + a manual `--serve --resume` smoke (Step 4). **Read first:** all of `web/app.js`, `web/style.css`, `web/index.html`.

**Interfaces — Consumes:** `GET /history` → `{"history": [ ...raw messages... ]}` (Task 2). Raw message shapes: `{role:"user",content:str}`, `{role:"assistant",content:str}`, `{role:"assistant",content:null,tool_calls:[{function:{name,arguments(STRING)}}]}`, `{role:"tool",tool_call_id,content(STRING)}`.

- [ ] **Step 1: Render helpers + fetch-on-load.** In `web/app.js`, add a cap constant near the top (after the `const input = ...` line):

```js
const TOOL_RESULT_MAX = 500;  // dim tool-result entries are truncated for display
```

Add these functions (after `addConfirm`, before `postJson`):

```js
function addToolCall(name, args) {
  const d = document.createElement('div');
  d.className = 'msg tool-call';
  const a = (typeof args === 'string') ? args : JSON.stringify(args);
  d.innerHTML = '<span class="label">\u{1F527} ' + escapeText(name) + ' </span>' + escapeText(a);
  log.appendChild(d);
  scrollDown();
}
function addToolResult(content) {
  const d = document.createElement('div');
  d.className = 'msg tool-result';
  let s = (typeof content === 'string') ? content : JSON.stringify(content);
  if (s.length > TOOL_RESULT_MAX) s = s.slice(0, TOOL_RESULT_MAX) + '…';
  d.innerHTML = '<span class="label">→ </span>' + escapeText(s);
  log.appendChild(d);
  scrollDown();
}
function renderHistory(msgs) {
  for (const m of msgs) {
    if (!m || typeof m !== 'object') continue;
    if (m.role === 'user' && typeof m.content === 'string') {
      addMessage('user', m.content);
    } else if (m.role === 'assistant' && typeof m.content === 'string' && m.content.length) {
      addMessage('assistant', m.content);
    } else if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      for (const tc of m.tool_calls) {
        const fn = (tc && tc.function) ? tc.function : {};
        addToolCall(fn.name || '?', fn.arguments != null ? fn.arguments : '');
      }
    } else if (m.role === 'tool') {
      addToolResult(m.content != null ? m.content : '');
    }
    // any other shape: skip (tolerant; never throw)
  }
}
async function loadHistory() {
  try {
    const r = await fetch('/history', {headers: {'X-Hades': '1'}});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const data = await r.json();
    if (data && Array.isArray(data.history)) renderHistory(data.history);
  } catch (e) {
    addError('history load failed: ' + e.message);
  }
}
```

At the very end of `web/app.js` (after the `form.addEventListener('submit', ...)` block), call it once on load:

```js
loadHistory();
```

- [ ] **Step 2: Dim tool styling.** In `web/style.css`, add after the `.msg.error` rule (line 29):

```css
.msg.tool-call{color:var(--muted); font-size:13px;}
.msg.tool-call .label{color:var(--confirm); font-weight:700;}
.msg.tool-result{color:var(--muted); font-size:13px; opacity:.85;}
.msg.tool-result .label{color:var(--muted); font-weight:700;}
```

- [ ] **Step 3: Build (no code build needed; confirm the C++ suite still green).** The web files are static — no compile. Confirm nothing else broke:
```
nix develop --command ctest --test-dir build
```
Expected: **204** tests pass (unchanged from Task 2 — this task touches only `web/`).

- [ ] **Step 4: Manual smoke (needs an API key; document the result in the report).**
```
export HADES_API_KEY=<key>
# 1) Seed a session by chatting once (writes .hades/sessions/<id>.jsonl):
nix develop --command ./build/hades manifests/dev.hades --serve 8080
#    open http://localhost:8080/ , send "read the file ./README.md and summarize", let a tool turn happen, then Ctrl-C
# 2) Resume it in the browser:
nix develop --command ./build/hades manifests/dev.hades --serve 8080 --resume
#    open http://localhost:8080/  -> the prior user message, the dim tool-call/tool-result, and the
#    assistant answer must all render on load (page is NOT blank).
# 3) CSRF gate (separate terminal):
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/history            # expect 403
curl -s -H 'X-Hades: 1' http://localhost:8080/history | head -c 200               # expect {"history":[...]}
# 4) Fresh (no --resume) still renders blank:
nix develop --command ./build/hades manifests/dev.hades --serve 8080
#    open http://localhost:8080/ -> empty log, fully usable.
```
If no key is available, state that the manual smoke was not run and the change is inspection-only; the reviewer should note it.

- [ ] **Step 5: Commit.**
```
git add web/app.js web/style.css
git commit -m "feat: web UI renders resumed conversation on load (GET /history, dim tool turns)"
```

---

## Self-Review (against the spec)

- **Coverage:** `read_session_jsonl` tolerant reader (T1); dedup `load_history` (T1, optional-but-taken, gated on 197 green); `GET /history` disk-read endpoint + `history_json` socket-free seam (T2); CSRF gate on `/history` via promoted `authorize` (T2); serve session-path wiring (T2); fetch-on-load + user/assistant/tool-call/tool-result render (T3); dim styling (T3); truncate long tool-result (T3); blank-on-fresh + no-regression (T1 backward-compat constraint + T3 Step 4.4).
- **No orphan-strip in display reader** — explicit test `KeepsTrailingToolCallOrphan` (T1).
- **Threading** — `history_json` reads disk only, no `mu_`; documented in code + plan.
- **Type consistency:** `read_session_jsonl(const std::string&) -> std::vector<nlohmann::json>` used identically in T1 (arbiter), T2 (`history_json`). `history_json()` returns `{"history": [...]}`; `app.js` reads `data.history` (T3). `authorize(const httplib::Request&) -> bool` static, called as `HttpServerModule::authorize` in listen() + tests. Stored `tool_calls[i].function.arguments` and `tool.content` are JSON **strings** → rendered via `escapeText` as-is/truncated (T3).
- **Backward-compat:** existing 197 stay green (T1 refactor identical parse; T2/T3 additive). New totals 201 (T1) → 204 (T2) → 204 (T3).

## Verification

1. Full suite green at each task (201 → 204 → 204).
2. `read_session_jsonl` tolerant of blank/corrupt/partial/non-object; missing file → `[]`.
3. `GET /history` returns stored messages; no `X-Hades` → 403; unset path → `{"history":[]}`.
4. Manual: `--serve --resume` renders the prior transcript (incl. dim tool turns); fresh `--serve` blank; curl CSRF 403/200.
