# MCP Discovery + HTTP Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** MCP servers rostered in the manifest get their tools DISCOVERED (`tools/list`) and announced to the LLM as `<block>__<tool>`, callable over two transports — stdio (existing, one-shot) and native Streamable HTTP with Bearer auth — gated by a new `mcp_allow` capability scope (default confirm).

**Architecture:** `ToolEntry` grows kind `"mcp_http"` + `api_key_env`; the MCP section of `src/apps/tool_runner/tool_runner.cpp` gains a transport-agnostic `mcp_list`/`mcp_call` pair (stdio = generalized one-shot conversation; HTTP = cpr POSTs with `Mcp-Session-Id` + SSE-or-JSON response parse). `ToolRegistry::ensure_warm` discovers per server, pushes prefixed `ToolSpec`s (flow to the LLM through the existing `wire_agent` → `set_tools` path, zero Arbiter changes) and keeps a prefixed→real-name map. `capability_of` maps any `__`-containing name to `Capability::McpTool`; `CapabilityPolicy` allows names listed in `mcp_allow` (or `*`), else confirms.

**Tech Stack:** C++20, nlohmann_json, libcpr (already linked in hades_core), httplib (tests only), GoogleTest, CMake+Ninja inside `nix develop`.

Spec: `docs/superpowers/specs/2026-07-12-mcp-discovery-design.md` (committed `9250cf2`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build` then `nix develop --command ctest --test-dir build --output-on-failure`. Baseline **626/626 green** before Task 1.
- Branch `feat/mcp-discovery` (already created; spec committed). Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- Prefix separator is exactly `__` (double underscore): announced name = `<block>__<real MCP tool name>`. Never string-split to recover the real name — always the registry map.
- MCP block names (`mcp`/`mcp_url` blocks) must match `[A-Za-z0-9_-]{1,64}` and contain no `__` → `MalConfig` at launch. Exactly ONE of `native|mcp|mcp_url` per Tool block → `MalConfig` otherwise.
- `mcp_allow` is whitespace-separated exact prefixed names; the single literal `*` allows all MCP tools. Default (absent/unlisted) = confirm.
- Discovery fail-soft: `tools/list` failure/empty → that entry keeps TODAY'S behavior (route by block name, no specs) + one stderr log line. Boot never blocked beyond the entry's timeout.
- HTTP transport: `Accept: application/json, text/event-stream`; Bearer only when `api_key_env` resolves non-empty; redirects OFF (`cpr::Redirect{false}` — the http_fetch precedent); response may be SSE-framed (`data:` lines) or plain JSON; best-effort `DELETE` teardown when a session id was issued. No SSRF gate on `mcp_url` (operator-set; documented).
- **Deviation from the spec's Files table (standing security constraint governs):** `manifests/dev.hades` is NOT edited/staged/committed (user's live uncommitted file — NEVER stage it, nor `manifests/pi.hades`, `memory/facts.md`, nor untracked `build-tsan/`, `manifests/dev2.hades`, `skills/greek-greeting/`, `skills/ponytail/`). The paste-ready example blocks ship inside `docs/manifest-reference.md` §4 instead (Task 5).
- Error strings in code blocks are exact (tests assert substrings).
- File headers: `// path — one-line purpose` + short explanation block (house style).

---

## File Structure

```
include/hades/tool/registry.h              T1 ToolEntry.api_key_env + kind "mcp_http" + mcp_real_name()
include/hades/tool/mcp_adapter.h           T1 re-signature: mcp_list/mcp_call over ToolEntry
src/apps/tool_runner/tool_runner.cpp       T1 stdio exchange generalized + mcp_list; T2 http_exchange; T3 discovery + call-path
tests/fake_mcp_server.sh                   T1 canned stdio MCP server (method-switched replies)
tests/test_mcp_adapter.cpp                 T1 stdio tests; T2 http tests (in-test httplib server)
tests/test_mcp_discovery.cpp               T3 registry discovery + ToolRunner e2e
include/hades/objective/capability_policy.h T4 enum McpTool + CapabilityScope.mcp_allow
src/behaviors/capability_policy.cpp        T4 capability_of __ rule + veto case
app/agent_wiring.cpp                       T4 Tool-block validation + mcp_allow parse
tests/test_capability_policy.cpp           T4 (append)
tests/test_wiring_mcp.cpp                  T4 MalConfig tests
docs/manifest-reference.md, CLAUDE.md      T5 ship docs
CMakeLists.txt                             T1, T3, T4 (test targets + compile defs)
```

---

## Task 1: Transport seam + stdio `mcp_list` (fake-server tests)

**Files:**
- Modify: `include/hades/tool/registry.h` (ToolEntry + accessor + map member)
- Modify: `include/hades/tool/mcp_adapter.h` (full replacement below)
- Modify: `src/apps/tool_runner/tool_runner.cpp` (MCP section rewrite; ToolRunner callsite)
- Create: `tests/fake_mcp_server.sh` (executable)
- Create: `tests/test_mcp_adapter.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ToolEntry{name, kind("native"|"mcp"|"mcp_http"), command, api_key_env, timeout_s}`; `ToolRegistry::add_from_block` parses `mcp_url` (kind `"mcp_http"`, `command` = the URL) and `api_key_env`; `std::string ToolRegistry::mcp_real_name(const std::string& prefixed) const` (empty when unknown — populated in Task 3).
- Produces: `nlohmann::json hades::mcp_list(const ToolEntry& server, double timeout_s = 30.0)` and `nlohmann::json hades::mcp_call(const ToolEntry& server, const std::string& tool, const nlohmann::json& args, double timeout_s = 30.0)` — stdio fully working; `"mcp_http"` kind returns `{"error":"mcp: http transport not built"}` until Task 2 replaces the stub body.
- Produces: `tests/fake_mcp_server.sh` driven by env vars `FAKE_MCP_LIST_REPLY` / `FAKE_MCP_CALL_REPLY`; CMake compile def `FAKE_MCP_SERVER` = its absolute source path.

- [ ] **Step 1: Baseline.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → 626/626.

- [ ] **Step 2: Create the fake server** `tests/fake_mcp_server.sh` (then `chmod +x tests/fake_mcp_server.sh`):

```sh
#!/bin/sh
# tests/fake_mcp_server.sh — canned one-shot MCP stdio server for tests.
#
# Reads the whole JSON-RPC conversation from stdin (run_subprocess closes the child's stdin
# after writing — src/core/subprocess.cpp:75), then emits a canned initialize result followed
# by the reply matching the request method: tools/call -> $FAKE_MCP_CALL_REPLY, anything else
# (tools/list) -> $FAKE_MCP_LIST_REPLY. Env-driven so each test picks its own wire replies.
input=$(cat)
case "$input" in
  *tools/call*) reply="$FAKE_MCP_CALL_REPLY" ;;
  *)            reply="$FAKE_MCP_LIST_REPLY" ;;
esac
echo '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05"}}'
printf '%s\n' "$reply"
```

- [ ] **Step 3: Write the failing tests** `tests/test_mcp_adapter.cpp`:

```cpp
// tests/test_mcp_adapter.cpp — mcp_list/mcp_call over both transports (stdio here; http in T2)
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/mcp_adapter.h"
#include "hades/tool/registry.h"
using namespace hades;

static ToolEntry stdio_entry(const std::string& cmd) {
  ToolEntry e;
  e.name = "weather";
  e.kind = "mcp";
  e.command = cmd;
  return e;
}

TEST(McpAdapter, RegistryParsesMcpUrlBlock) {
  Block b;
  b.name = "linear";
  b.kv["mcp_url"] = "https://mcp.example/mcp";
  b.kv["api_key_env"] = "LINEAR_MCP_KEY";
  b.kv["timeout_s"] = "60";
  ToolRegistry reg;
  reg.add_from_block(b);
  ASSERT_EQ(reg.entries().size(), 1u);
  EXPECT_EQ(reg.entries()[0].kind, "mcp_http");
  EXPECT_EQ(reg.entries()[0].command, "https://mcp.example/mcp");
  EXPECT_EQ(reg.entries()[0].api_key_env, "LINEAR_MCP_KEY");
  EXPECT_DOUBLE_EQ(reg.entries()[0].timeout_s, 60.0);
}

TEST(McpAdapter, StdioListReturnsTools) {
  ::setenv("FAKE_MCP_LIST_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"get_alerts",)"
           R"("description":"weather alerts","inputSchema":{"type":"object"}}]}})", 1);
  auto r = mcp_list(stdio_entry(FAKE_MCP_SERVER), 10.0);
  ASSERT_TRUE(r.is_object()) << r.dump();
  ASSERT_TRUE(r.contains("tools")) << r.dump();
  ASSERT_EQ(r["tools"].size(), 1u);
  EXPECT_EQ(r["tools"][0].value("name", ""), "get_alerts");
}

TEST(McpAdapter, StdioCallReturnsResult) {
  ::setenv("FAKE_MCP_CALL_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"sunny"}]}})", 1);
  auto r = mcp_call(stdio_entry(FAKE_MCP_SERVER), "get_alerts", {{"state", "CA"}}, 10.0);
  ASSERT_TRUE(r.is_object() && r.contains("content")) << r.dump();
  EXPECT_FALSE(r.contains("error"));
}

TEST(McpAdapter, StdioGarbageReplyIsError) {
  ::setenv("FAKE_MCP_LIST_REPLY", "not json at all", 1);
  auto r = mcp_list(stdio_entry(FAKE_MCP_SERVER), 10.0);
  ASSERT_TRUE(r.is_object());
  EXPECT_TRUE(r.contains("error"));
}

TEST(McpAdapter, EmptyCommandIsError) {
  auto r = mcp_list(stdio_entry(""), 10.0);
  EXPECT_TRUE(r.contains("error"));
}

TEST(McpAdapter, DeadCommandIsError) {
  auto r = mcp_call(stdio_entry("/nonexistent/mcp-server"), "x", {}, 5.0);
  EXPECT_TRUE(r.contains("error"));
}

TEST(McpAdapter, HttpKindIsStubbedUntilT2) {
  ToolEntry e;
  e.name = "linear";
  e.kind = "mcp_http";
  e.command = "https://mcp.example/mcp";
  auto r = mcp_list(e, 5.0);
  EXPECT_TRUE(r.contains("error"));   // T2 replaces the stub; error contract holds either way
}
```

- [ ] **Step 4: CMake.** In `CMakeLists.txt`, next to the other test-source adds:

```cmake
target_sources(hades_tests PRIVATE tests/test_mcp_adapter.cpp)
target_compile_definitions(hades_tests PRIVATE
  FAKE_MCP_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_mcp_server.sh")
```

- [ ] **Step 5: Run — expect FAIL** (compile error: `mcp_list` undeclared; `api_key_env` missing):
`nix develop --command cmake --build build 2>&1 | tail -20`

- [ ] **Step 6: Implement.**

**(a)** `include/hades/tool/registry.h` — in `struct ToolEntry`, replace the `kind`/`command` lines with:

```cpp
  std::string kind;     // "native" | "mcp" (stdio command) | "mcp_http" (Streamable HTTP url)
  std::string command;  // native/mcp: argv string (whitespace-split at spawn) | mcp_http: the URL
  std::string api_key_env;  // mcp_http only: env var holding the Bearer token ("" = no auth header)
```

and in `class ToolRegistry` (public, after `find_by_tool_name`):

```cpp
  // Real (unprefixed) MCP tool name behind a discovered `<block>__<tool>` announce name.
  // Empty when `prefixed` was not produced by MCP discovery (legacy call-by-block-name path).
  std::string mcp_real_name(const std::string& prefixed) const;
```

and (private, after `by_tool_name_`):

```cpp
  // discovered prefixed name -> the server's own tool name (what tools/call must send).
  mutable std::map<std::string, std::string> mcp_real_names_;
```

**(b)** `include/hades/tool/mcp_adapter.h` — full replacement:

```cpp
// include/hades/tool/mcp_adapter.h — MCP client (stdio + Streamable HTTP) for ToolRunner
//
// Transport-agnostic MCP exchange over a configured server entry: kind "mcp" = one-shot stdio
// (spawn command, newline-delimited JSON-RPC), kind "mcp_http" = Streamable HTTP (cpr POSTs,
// optional Bearer from entry.api_key_env, Mcp-Session-Id lifecycle, SSE-or-JSON responses).
// mcp_list -> the server's tools/list result ({"tools":[...]}); mcp_call -> one tools/call
// result. Both never throw; timeout / transport / malformed-reply -> {"error": "..."}.

#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/registry.h"   // ToolEntry (the transport descriptor)
namespace hades {
nlohmann::json mcp_list(const ToolEntry& server, double timeout_s = 30.0);
nlohmann::json mcp_call(const ToolEntry& server, const std::string& tool,
                        const nlohmann::json& args, double timeout_s = 30.0);
}  // namespace hades
```

**(c)** `src/apps/tool_runner/tool_runner.cpp` — replace the whole `mcp_call` section (everything after the `// ── mcp_call: …` banner) with:

```cpp
// ── MCP client: stdio + Streamable HTTP transports behind mcp_list/mcp_call ──────────────
namespace hades {
namespace {

// One-shot stdio conversation: initialize + initialized-notification + the request (id 2),
// newline-delimited on the spawned server's stdin; reply scanned off stdout.
std::string stdio_conversation(const nlohmann::json& request) {
  std::ostringstream in;
  in << nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", "2024-11-05"},
                         {"capabilities", nlohmann::json::object()},
                         {"clientInfo", {{"name", "hades"}, {"version", "0.1.0"}}}}}}
            .dump()
     << "\n";
  in << nlohmann::json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump()
     << "\n";
  in << request.dump() << "\n";
  return in.str();
}

nlohmann::json stdio_exchange(const std::string& command, const nlohmann::json& request,
                              double timeout_s) {
  if (command.empty()) return {{"error", "mcp: empty command"}};
  auto r = run_subprocess(split_ws(command), stdio_conversation(request), timeout_s);
  if (r.timed_out) return {{"error", "mcp server timed out"}};
  // Scan stdout lines for the JSON-RPC reply to our request (id == 2). Every JSON access is
  // guarded so a malformed server line can never throw.
  std::istringstream out(r.out);
  std::string line;
  while (std::getline(out, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_object() && j.contains("id") && j["id"] == 2 && j.contains("result"))
      return j["result"];
  }
  return {{"error", "mcp: no result for request"}};
}

// Streamable HTTP transport — implemented in Task 2. The stub keeps the error contract so
// callers (and the T1 test) see a plain {"error"} rather than a crash or an empty object.
nlohmann::json http_exchange(const ToolEntry& server, const nlohmann::json& request,
                             double timeout_s);

nlohmann::json exchange(const ToolEntry& server, const nlohmann::json& request,
                        double timeout_s) {
  if (server.kind == "mcp_http") return http_exchange(server, request, timeout_s);
  return stdio_exchange(server.command, request, timeout_s);
}

nlohmann::json http_exchange(const ToolEntry&, const nlohmann::json&, double) {
  return {{"error", "mcp: http transport not built"}};
}

}  // namespace

nlohmann::json mcp_list(const ToolEntry& server, double timeout_s) {
  return exchange(server,
                  nlohmann::json{{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "tools/list"},
                                 {"params", nlohmann::json::object()}},
                  timeout_s);
}

nlohmann::json mcp_call(const ToolEntry& server, const std::string& tool,
                        const nlohmann::json& args, double timeout_s) {
  return exchange(server,
                  nlohmann::json{{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "tools/call"},
                                 {"params", {{"name", tool}, {"arguments", args}}}},
                  timeout_s);
}

}  // namespace hades
```

**(d)** Same file, ToolRunner subscribe handler — the old callsite

```cpp
    } else {  // mcp
      content = mcp_call(te->command, name, args, timeout);
```

becomes (real-name resolution arrives in Task 3; passing `name` through preserves today's behavior):

```cpp
    } else {  // mcp | mcp_http
      content = mcp_call(*te, name, args, timeout);
```

**(e)** Same file, `ToolRegistry::add_from_block` — replace the kind-selection lines with:

```cpp
  if (b.kv.count("native"))        { e.kind = "native";   e.command = b.kv.at("native"); }
  else if (b.kv.count("mcp"))      { e.kind = "mcp";      e.command = b.kv.at("mcp"); }
  else if (b.kv.count("mcp_url"))  { e.kind = "mcp_http"; e.command = b.kv.at("mcp_url"); }
  else return;                         // unchanged behavior: a block with none is ignored
  if (b.kv.count("api_key_env")) e.api_key_env = b.kv.at("api_key_env");
```

**(f)** Same file, add the accessor next to `find_by_tool_name`:

```cpp
std::string ToolRegistry::mcp_real_name(const std::string& prefixed) const {
  auto it = mcp_real_names_.find(prefixed);
  return it != mcp_real_names_.end() ? it->second : std::string{};
}
```

- [ ] **Step 7: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R McpAdapter` → 7/7 pass. Full suite → **633/633**.

- [ ] **Step 8: Commit.**

```bash
git add include/hades/tool/registry.h include/hades/tool/mcp_adapter.h src/apps/tool_runner/tool_runner.cpp tests/fake_mcp_server.sh tests/test_mcp_adapter.cpp CMakeLists.txt
git commit -m "feat: MCP transport seam — ToolEntry mcp_http kind, mcp_list, stdio exchange generalized"
```

---

## Task 2: Streamable HTTP exchange (cpr)

**Files:**
- Modify: `src/apps/tool_runner/tool_runner.cpp` (replace the `http_exchange` stub; add includes)
- Modify: `tests/test_mcp_adapter.cpp` (append HTTP tests)

**Interfaces:**
- Consumes: `ToolEntry{kind:"mcp_http", command:<url>, api_key_env}`, the `exchange()` dispatch and error contract from Task 1.
- Produces: working `http_exchange` — POST `initialize` (capture `Mcp-Session-Id` response header) → POST `notifications/initialized` (best-effort) → POST the request → parse plain-JSON or SSE-framed reply → best-effort `DELETE` teardown. Bearer header from `api_key_env` env. Errors: `"mcp: http error: …"` (transport), `"mcp: http <status> …"` (≥400), `"mcp: no result in event stream"`, `"mcp: malformed http response"`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_mcp_adapter.cpp` (add includes `<httplib.h>`, `<thread>` at top; httplib is already a test dep — precedent `tests/test_ask_agent_tool.cpp`; if `wait_until_ready()` is not what that file uses for server readiness, mirror its exact idiom instead):

```cpp
// ── HTTP transport tests: real cpr client against an in-test httplib server ──
namespace {

// Minimal scripted Streamable-HTTP MCP server. Issues Mcp-Session-Id on initialize, records
// the session id + Authorization it sees on the tools request, answers JSON or SSE.
struct FakeHttpMcp {
  httplib::Server srv;
  std::thread th;
  int port = 0;
  bool sse = false;                 // answer the tools request as text/event-stream
  int fail_status = 0;              // non-zero: initialize answers this status
  std::string seen_session, seen_auth, list_result =
      R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"search_issues",)"
      R"("description":"search","inputSchema":{"type":"object"}}]}})";
  bool got_delete = false;

  FakeHttpMcp() {
    srv.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
      auto j = nlohmann::json::parse(req.body, nullptr, false);
      const std::string method = j.is_object() ? j.value("method", "") : "";
      if (method == "initialize") {
        if (fail_status) { res.status = fail_status; return; }
        res.set_header("Mcp-Session-Id", "sess-42");
        res.set_content(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05"}})",
                        "application/json");
      } else if (method == "notifications/initialized") {
        res.status = 202;
      } else {   // tools/list | tools/call
        seen_session = req.get_header_value("Mcp-Session-Id");
        seen_auth = req.get_header_value("Authorization");
        if (sse)
          res.set_content("event: message\ndata: " + list_result + "\n\n", "text/event-stream");
        else
          res.set_content(list_result, "application/json");
      }
    });
    srv.Delete("/mcp", [this](const httplib::Request&, httplib::Response& res) {
      got_delete = true;
      res.status = 200;
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~FakeHttpMcp() {
    srv.stop();
    th.join();
  }
  hades::ToolEntry entry(const std::string& key_env = "") {
    hades::ToolEntry e;
    e.name = "linear";
    e.kind = "mcp_http";
    e.command = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    e.api_key_env = key_env;
    return e;
  }
};

}  // namespace

TEST(McpAdapterHttp, JsonReplyAndSessionEcho) {
  FakeHttpMcp fake;
  auto r = mcp_list(fake.entry(), 10.0);
  ASSERT_TRUE(r.contains("tools")) << r.dump();
  EXPECT_EQ(r["tools"][0].value("name", ""), "search_issues");
  EXPECT_EQ(fake.seen_session, "sess-42");     // session header echoed on the request
  EXPECT_TRUE(fake.seen_auth.empty());         // no api_key_env -> no Authorization header
  EXPECT_TRUE(fake.got_delete);                // best-effort session teardown
}

TEST(McpAdapterHttp, SseFramedReplyParsed) {
  FakeHttpMcp fake;
  fake.sse = true;
  auto r = mcp_list(fake.entry(), 10.0);
  ASSERT_TRUE(r.contains("tools")) << r.dump();
}

TEST(McpAdapterHttp, BearerHeaderFromEnv) {
  ::setenv("TEST_MCP_KEY", "sekrit", 1);
  FakeHttpMcp fake;
  auto r = mcp_list(fake.entry("TEST_MCP_KEY"), 10.0);
  ASSERT_TRUE(r.contains("tools"));
  EXPECT_EQ(fake.seen_auth, "Bearer sekrit");
}

TEST(McpAdapterHttp, AuthFailureSurfacesStatus) {
  FakeHttpMcp fake;
  fake.fail_status = 401;
  auto r = mcp_list(fake.entry(), 10.0);
  ASSERT_TRUE(r.contains("error"));
  EXPECT_NE(r["error"].get<std::string>().find("401"), std::string::npos);
}

TEST(McpAdapterHttp, CallGoesThroughSameTransport) {
  FakeHttpMcp fake;
  fake.list_result =
      R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"done"}]}})";
  auto r = mcp_call(fake.entry(), "search_issues", {{"q", "bug"}}, 10.0);
  ASSERT_TRUE(r.contains("content")) << r.dump();
}

TEST(McpAdapterHttp, UnreachableServerIsError) {
  hades::ToolEntry e;
  e.name = "linear";
  e.kind = "mcp_http";
  e.command = "http://127.0.0.1:9/mcp";   // port 9 (discard) — nothing listens
  auto r = mcp_list(e, 5.0);
  EXPECT_TRUE(r.contains("error"));
}
```

- [ ] **Step 2: Run — expect FAIL** (`-R McpAdapterHttp` — all return the T1 stub error).

- [ ] **Step 3: Implement.** In `src/apps/tool_runner/tool_runner.cpp`: add includes near the top (`#include <cpr/cpr.h>`, `#include <cstdlib>` if absent). Replace the stub `http_exchange` definition with:

```cpp
// Extract the id==2 JSON-RPC payload from an HTTP response body that is either plain JSON or
// a one-off SSE stream (a Streamable-HTTP server MAY answer any POST as text/event-stream —
// scan `data:` lines for the object with our id). Guarded parse: malformed lines are skipped.
nlohmann::json parse_http_rpc(const std::string& content_type, const std::string& body) {
  auto pick = [](const nlohmann::json& j) -> nlohmann::json {
    if (j.is_object() && j.contains("id") && j["id"] == 2) {
      if (j.contains("result")) return j["result"];
      if (j.contains("error") && j["error"].is_object())
        return nlohmann::json{
            {"error", "mcp: server error: " + j["error"].value("message", std::string{"unknown"})}};
    }
    return nlohmann::json();   // null = not the reply we asked for
  };
  if (content_type.find("text/event-stream") != std::string::npos) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) != 0) continue;
      std::string payload = line.substr(5);
      if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
      auto r = pick(nlohmann::json::parse(payload, nullptr, false));
      if (!r.is_null()) return r;
    }
    return {{"error", "mcp: no result in event stream"}};
  }
  auto r = pick(nlohmann::json::parse(body, nullptr, false));
  if (!r.is_null()) return r;
  return {{"error", "mcp: malformed http response"}};
}

// Streamable HTTP exchange: initialize (capture Mcp-Session-Id) -> initialized notification
// (best-effort) -> the request -> best-effort DELETE teardown. Bearer comes from the entry's
// api_key_env (env-only, never logged). Redirects OFF (http_fetch precedent). The url is
// OPERATOR-set in the manifest (not LLM-chosen), so no private-net gate applies here.
nlohmann::json http_exchange(const ToolEntry& server, const nlohmann::json& request,
                             double timeout_s) {
  const std::string& url = server.command;
  if (url.empty()) return {{"error", "mcp: empty url"}};
  std::string bearer;
  if (!server.api_key_env.empty())
    if (const char* v = std::getenv(server.api_key_env.c_str()); v && *v) bearer = v;
  auto headers = [&](const std::string& session) {
    cpr::Header h{{"Content-Type", "application/json"},
                  {"Accept", "application/json, text/event-stream"}};
    if (!bearer.empty()) h["Authorization"] = "Bearer " + bearer;
    if (!session.empty()) h["Mcp-Session-Id"] = session;
    return h;
  };
  const cpr::Timeout t{static_cast<long>(timeout_s * 1000)};

  nlohmann::json init{{"jsonrpc", "2.0"},
                      {"id", 1},
                      {"method", "initialize"},
                      {"params",
                       {{"protocolVersion", "2024-11-05"},
                        {"capabilities", nlohmann::json::object()},
                        {"clientInfo", {{"name", "hades"}, {"version", "0.1.0"}}}}}};
  auto r1 = cpr::Post(cpr::Url{url}, headers(""), cpr::Body{init.dump()}, t,
                      cpr::Redirect{false});
  if (r1.error.code != cpr::ErrorCode::OK)
    return {{"error", "mcp: http error: " + r1.error.message}};
  if (r1.status_code >= 400)
    return {{"error", "mcp: http " + std::to_string(r1.status_code) + " on initialize"}};
  std::string session;
  if (auto it = r1.header.find("Mcp-Session-Id"); it != r1.header.end()) session = it->second;

  cpr::Post(cpr::Url{url}, headers(session),
            cpr::Body{nlohmann::json{{"jsonrpc", "2.0"},
                                     {"method", "notifications/initialized"}}.dump()},
            t, cpr::Redirect{false});   // best-effort; spec says 202

  auto r3 = cpr::Post(cpr::Url{url}, headers(session), cpr::Body{request.dump()}, t,
                      cpr::Redirect{false});
  nlohmann::json out;
  if (r3.error.code != cpr::ErrorCode::OK)
    out = {{"error", "mcp: http error: " + r3.error.message}};
  else if (r3.status_code >= 400)
    out = {{"error", "mcp: http " + std::to_string(r3.status_code)}};
  else {
    std::string ct;
    if (auto it = r3.header.find("Content-Type"); it != r3.header.end()) ct = it->second;
    out = parse_http_rpc(ct, r3.text);
  }
  if (!session.empty())
    cpr::Delete(cpr::Url{url}, headers(session), t, cpr::Redirect{false});   // best-effort
  return out;
}
```

Delete the forward declaration + stub pair from Task 1 (the real definition must appear BEFORE `exchange()` uses it, or keep the forward declaration and place the definition after — either compiles; keep the forward declaration and drop only the stub body).

Note: `cpr::Header` is case-insensitive on lookup (`r1.header.find("Mcp-Session-Id")` matches any case the server sent — httplib sends the exact case we set). `HttpKindIsStubbedUntilT2` from Task 1 now hits a real transport against `https://mcp.example/mcp` — DNS for `.example` fails fast → still `{"error"}` → the test still passes as written (its assertion is transport-agnostic by design).

- [ ] **Step 4: Build + test.** `-R McpAdapter` (covers both suites) → all pass. Full suite → **639/639**.

- [ ] **Step 5: Commit.**

```bash
git add src/apps/tool_runner/tool_runner.cpp tests/test_mcp_adapter.cpp
git commit -m "feat: MCP Streamable HTTP transport — Bearer auth, Mcp-Session-Id, SSE-or-JSON replies"
```

---

## Task 3: Discovery in the registry + ToolRunner real-name call path

**Files:**
- Modify: `src/apps/tool_runner/tool_runner.cpp` (`ensure_warm` mcp branch; ToolRunner callsite)
- Create: `tests/test_mcp_discovery.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `mcp_list`/`mcp_call` (T1/T2), `mcp_real_names_`/`mcp_real_name()` (T1), `FAKE_MCP_SERVER` compile def (T1).
- Produces: discovered `ToolSpec{ "<block>__<name>", description, inputSchema }` entries in `specs_` (flow to the LLM via the existing `wire_agent` → `set_tools` path untouched); `by_tool_name_[prefixed]` routing; populated `mcp_real_names_`; fail-soft legacy fallback.

- [ ] **Step 1: Write the failing tests** `tests/test_mcp_discovery.cpp`:

```cpp
// tests/test_mcp_discovery.cpp — registry MCP discovery + ToolRunner end-to-end (stdio fake)
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/module/tool_runner.h"
#include "hades/tool/registry.h"
using namespace hades;

static Block mcp_block(const std::string& name, const std::string& cmd) {
  Block b;
  b.name = name;
  b.kv["mcp"] = cmd;
  return b;
}
static const char* kList =
    R"({"jsonrpc":"2.0","id":2,"result":{"tools":[)"
    R"({"name":"get_alerts","description":"weather alerts","inputSchema":{"type":"object",)"
    R"("properties":{"state":{"type":"string"}}}},)"
    R"({"name":"get_forecast","description":"forecast","inputSchema":{"type":"object"}}]}})";

TEST(McpDiscovery, DiscoveredToolsAnnouncedPrefixed) {
  ::setenv("FAKE_MCP_LIST_REPLY", kList, 1);
  ToolRegistry reg;
  reg.add_from_block(mcp_block("weather", FAKE_MCP_SERVER));
  auto specs = reg.specs(10.0);
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[0].name, "weather__get_alerts");
  EXPECT_EQ(specs[0].description, "weather alerts");
  EXPECT_EQ(specs[0].schema.value("type", ""), "object");
  EXPECT_TRUE(specs[0].schema.contains("properties"));   // inputSchema passes through 1:1
  EXPECT_EQ(specs[1].name, "weather__get_forecast");
}

TEST(McpDiscovery, PrefixedNameRoutesAndRealNameMaps) {
  ::setenv("FAKE_MCP_LIST_REPLY", kList, 1);
  ToolRegistry reg;
  reg.add_from_block(mcp_block("weather", FAKE_MCP_SERVER));
  reg.warm(10.0);
  const ToolEntry* te = reg.find_by_tool_name("weather__get_alerts");
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(te->kind, "mcp");
  EXPECT_EQ(reg.mcp_real_name("weather__get_alerts"), "get_alerts");
  EXPECT_EQ(reg.mcp_real_name("weather"), "");            // not a discovered name
}

TEST(McpDiscovery, FailSoftKeepsLegacyBlockNameRouting) {
  ToolRegistry reg;
  reg.add_from_block(mcp_block("deadsrv", "/nonexistent/mcp-server"));
  auto specs = reg.specs(5.0);
  EXPECT_TRUE(specs.empty());                             // nothing announced
  const ToolEntry* te = reg.find_by_tool_name("deadsrv"); // today's path still works
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(te->kind, "mcp");
}

TEST(McpDiscovery, ToolRunnerCallsRealNameEndToEnd) {
  ::setenv("FAKE_MCP_LIST_REPLY", kList, 1);
  ::setenv("FAKE_MCP_CALL_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"72F"}]}})", 1);
  Blackboard bb;
  ToolRunner tr;
  tr.add_tool(mcp_block("weather", FAKE_MCP_SERVER));
  Block cfg;
  tr.on_start(cfg, bb);   // warms -> discovery runs here
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.post("TOOL_REQUEST",
          {{"id", "m1"}, {"tool", "weather__get_alerts"}, {"args", {{"state", "CA"}}}},
          "arbiter");
  bb.pump();
  ASSERT_TRUE(result.is_object());
  EXPECT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_NE(result["content"].dump().find("72F"), std::string::npos);
}

TEST(McpDiscovery, EmptyToolNamesSkipped) {
  ::setenv("FAKE_MCP_LIST_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"","description":"x"},)"
           R"({"name":"ok_tool","description":"y","inputSchema":{"type":"object"}}]}})", 1);
  ToolRegistry reg;
  reg.add_from_block(mcp_block("srv", FAKE_MCP_SERVER));
  auto specs = reg.specs(10.0);
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].name, "srv__ok_tool");
}
```

(Check `ToolRunner`'s public surface in `include/hades/module/tool_runner.h` — `add_tool(const Block&)` and `registry()` exist; the e2e test mirrors how `wire_agent` drives it.)

- [ ] **Step 2: CMake + run — expect FAIL.**

```cmake
target_sources(hades_tests PRIVATE tests/test_mcp_discovery.cpp)
```

Discovery tests fail (`specs` empty — the mcp branch still skips); e2e fails (routes by block name, calls `weather__get_alerts` on the server).

- [ ] **Step 3: Implement.** In `src/apps/tool_runner/tool_runner.cpp`:

**(a)** `ensure_warm`, replace the `if (t.kind != "native") { … continue; }` branch with:

```cpp
    if (t.kind != "native") {
      // MCP discovery: one tools/list exchange per server. Each discovered tool announces as
      // <block>__<name> — the prefix guarantees a server can never shadow a native tool name,
      // and capability_of maps any "__" name to McpTool (mcp_allow-gated). tools/call needs
      // the server's OWN name, kept in mcp_real_names_ (never recovered by string-splitting).
      const double t_timeout = t.timeout_s > 0.0 ? t.timeout_s : timeout_s;
      auto listed = mcp_list(t, t_timeout);
      bool any = false;
      if (listed.is_object() && listed.contains("tools") && listed["tools"].is_array()) {
        for (const auto& disc : listed["tools"]) {
          if (!disc.is_object()) continue;
          const std::string real = disc.value("name", "");
          if (real.empty()) continue;
          const std::string prefixed = t.name + "__" + real;
          specs_.push_back({prefixed, disc.value("description", ""),
                            disc.contains("inputSchema") && disc["inputSchema"].is_object()
                                ? disc["inputSchema"]
                                : nlohmann::json::object()});
          by_tool_name_.emplace(prefixed, &t);
          mcp_real_names_.emplace(prefixed, real);
          any = true;
        }
      }
      if (!any) {
        // Fail-soft: discovery failed or returned nothing -> keep the legacy call-by-block-
        // name path so a down server degrades to pre-discovery behavior; boot is never
        // blocked beyond this entry's timeout.
        std::fprintf(stderr, "[hades] mcp discovery failed for '%s': %s\n", t.name.c_str(),
                     listed.is_object() && listed.contains("error")
                         ? listed["error"].dump().c_str()
                         : "no tools");
        by_tool_name_.emplace(t.name, &t);
      }
      continue;
    }
```

Add `#include <cstdio>` and `#include "hades/tool/mcp_adapter.h"` at the top if not already present (the registry section now calls `mcp_list`).

**(b)** ToolRunner callsite (from Task 1) becomes the real-name resolve:

```cpp
    } else {  // mcp | mcp_http
      const std::string real = reg_.mcp_real_name(name);
      content = mcp_call(*te, real.empty() ? name : real, args, timeout);
      ok = content.is_object() && !content.contains("error");
    }
```

- [ ] **Step 4: Build + test.** `-R McpDiscovery` → 5/5. Full suite → **644/644**.

- [ ] **Step 5: Commit.**

```bash
git add src/apps/tool_runner/tool_runner.cpp tests/test_mcp_discovery.cpp CMakeLists.txt
git commit -m "feat: MCP discovery — tools/list at warm, block__tool specs, real-name call path, fail-soft"
```

---

## Task 4: `Capability::McpTool` + `mcp_allow` + wiring validation

**Files:**
- Modify: `include/hades/objective/capability_policy.h` (enum + scope field)
- Modify: `src/behaviors/capability_policy.cpp` (`capability_of` + `veto` case)
- Modify: `app/agent_wiring.cpp` (`make_objective` mcp_allow parse; Tool-block validation pass)
- Modify: `tests/test_capability_policy.cpp` (append)
- Create: `tests/test_wiring_mcp.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: prefixed `<block>__<tool>` naming (T3); `CapabilityScope`/`CapabilityPolicy` as in `include/hades/objective/capability_policy.h`; `split_ws_list`, `MalConfig`, `valid_skill_name` (all already used/included in `app/agent_wiring.cpp`).
- Produces: `Capability::McpTool`; `CapabilityScope.mcp_allow` (`std::vector<std::string>`); veto = allow when listed or `*`, else confirm `"mcp tool outside mcp_allow: <name>"`; launch-time `MalConfig` for multi-kind Tool blocks and bad mcp block names.

- [ ] **Step 1: Write the failing tests.**

**(a)** Append to `tests/test_capability_policy.cpp`:

```cpp
TEST(CapabilityPolicy, McpToolDetectedByDoubleUnderscore) {
  EXPECT_EQ(CapabilityPolicy::capability_of("weather__get_alerts"), Capability::McpTool);
  EXPECT_EQ(CapabilityPolicy::capability_of("linear__search_issues"), Capability::McpTool);
  // Native names (single underscores) are untouched.
  EXPECT_EQ(CapabilityPolicy::capability_of("fs_read"), Capability::FsRead);
  EXPECT_EQ(CapabilityPolicy::capability_of("save_memory"), Capability::MemoryAppend);
}

TEST(CapabilityPolicy, McpToolConfirmsByDefault) {
  CapabilityScope sc;
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action a{Action::Kind::ToolCall};
  a.tool = "weather__get_alerts";
  auto v = p.veto(bb, a);
  EXPECT_TRUE(v.vetoed);
  EXPECT_TRUE(v.needs_confirm);
}

TEST(CapabilityPolicy, McpAllowExactNameAllows) {
  CapabilityScope sc;
  sc.mcp_allow = {"weather__get_forecast"};
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action ok{Action::Kind::ToolCall};
  ok.tool = "weather__get_forecast";
  EXPECT_FALSE(p.veto(bb, ok).vetoed);
  Action other{Action::Kind::ToolCall};
  other.tool = "weather__get_alerts";              // NOT listed -> still confirm
  EXPECT_TRUE(p.veto(bb, other).needs_confirm);
}

TEST(CapabilityPolicy, McpAllowStarAllowsAll) {
  CapabilityScope sc;
  sc.mcp_allow = {"*"};
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action a{Action::Kind::ToolCall};
  a.tool = "anysrv__anytool";
  EXPECT_FALSE(p.veto(bb, a).vetoed);
}
```

**(b)** Create `tests/test_wiring_mcp.cpp`:

```cpp
// tests/test_wiring_mcp.cpp — launch-time validation of MCP Tool blocks + mcp_allow parse
#include <gtest/gtest.h>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

static Manifest manifest_with(const std::string& tool_block) {
  return parse_manifest("Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n" +
                        tool_block);
}

TEST(McpWiring, MultiKindToolBlockThrows) {
  Blackboard bb;
  Manifest m = manifest_with(
      "Tool = weird\n{\n  native = ./build/hades-fs-read\n  mcp = ./srv\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(McpWiring, DoubleUnderscoreMcpBlockNameThrows) {
  Blackboard bb;
  Manifest m = manifest_with("Tool = we__ird { mcp = ./srv }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(McpWiring, BadCharsetMcpBlockNameThrows) {
  Blackboard bb;
  Manifest m = manifest_with("Tool = bad.name { mcp = ./srv }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(McpWiring, NativeBlockNamesUnaffected) {
  // A native tool block never hits the mcp name gate (no new constraint on native names).
  Blackboard bb;
  Manifest m = manifest_with("Tool = fs { native = ./build/hades-fs-read }\n");
  EXPECT_NO_THROW(build_agent(bb, m));
}

TEST(McpWiring, ValidMcpBlockBuilds) {
  // /nonexistent discovery fails soft (stderr line) — boot must still succeed.
  Blackboard bb;
  Manifest m = manifest_with("Tool = weather { mcp = /nonexistent/mcp-server }\n");
  EXPECT_NO_THROW(build_agent(bb, m));
}
```

- [ ] **Step 2: CMake + run — expect FAIL** (no `McpTool` enum; no validation):

```cmake
target_sources(hades_tests PRIVATE tests/test_wiring_mcp.cpp)
```

- [ ] **Step 3: Implement.**

**(a)** `include/hades/objective/capability_policy.h` — the enum becomes:

```cpp
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SkillRead, SkillWrite, PeerAsk, GitRead, ExecScoped, SelfSchedule, McpTool, Unknown };
```

and `CapabilityScope` gains (after `exec_allow`):

```cpp
  std::vector<std::string> mcp_allow;         // discovered MCP tool names (<block>__<tool>)
                                              // allowed WITHOUT confirm; the single literal "*"
                                              // allows all MCP tools (trusts every rostered
                                              // server). Whitespace-separated in the manifest.
```

**(b)** `src/behaviors/capability_policy.cpp` — in `capability_of`, directly before `return Capability::Unknown;`:

```cpp
  // Structural rule, not a name row: only MCP discovery produces "__" (native tool names use
  // single underscores; wiring rejects mcp BLOCK names containing "__"), so a double
  // underscore marks a foreign server's tool wherever it appears in the name.
  if (tool.find("__") != std::string::npos)              return Capability::McpTool;
```

In `veto`, before `case Capability::Unknown:`:

```cpp
    case Capability::McpTool: {
      // A discovered <block>__<tool> on a rostered MCP server: foreign code hades does not
      // control — shell-grade trust. The operator opts specific tools (or "*" = every
      // discovered tool) into the allow band via mcp_allow; everything else confirms, which
      // also auto-denies on heartbeat/peer turns (no unattended foreign-server execution).
      for (const auto& m : scope_.mcp_allow)
        if (m == "*" || m == a.tool) return allow();
      return confirm("mcp tool outside mcp_allow: " + a.tool);
    }
```

**(c)** `app/agent_wiring.cpp`:

In `make_objective`'s `capability_policy` branch, after the `exec_allow` line:

```cpp
    if (b.kv.count("mcp_allow"))      sc.mcp_allow      = split_ws_list(b.kv.at("mcp_allow"));
```

In `wire_agent`, at the TOP of the tool handling (immediately before the `tools_resolved` loop — launch boundary, before any side effects):

```cpp
  // MCP Tool-block validation (launch boundary — fail before any tool subprocess runs):
  // exactly one transport kind per block, and mcp block names must be prefix-safe — the
  // announce name is "<block>__<tool>", so a "__" inside the block name (or a char outside
  // [A-Za-z0-9_-]) would corrupt the prefix rule capability_of depends on. valid_skill_name
  // is the same [A-Za-z0-9_-]{1,64} gate (borrowed; skills and mcp blocks share the charset).
  for (const Block& t : tools) {
    const int kinds = (t.kv.count("native") ? 1 : 0) + (t.kv.count("mcp") ? 1 : 0) +
                      (t.kv.count("mcp_url") ? 1 : 0);
    if (kinds > 1)
      throw MalConfig("Tool block '" + t.name + "': exactly one of native|mcp|mcp_url");
    if ((t.kv.count("mcp") || t.kv.count("mcp_url")) &&
        (!valid_skill_name(t.name) || t.name.find("__") != std::string::npos))
      throw MalConfig("Tool block '" + t.name +
                      "': mcp block names must be [A-Za-z0-9_-]{1,64} without '__'");
  }
```

- [ ] **Step 4: Build + test.** `-R "CapabilityPolicy|McpWiring"` → all pass. Full suite → **653/653**.

- [ ] **Step 5: Commit.**

```bash
git add include/hades/objective/capability_policy.h src/behaviors/capability_policy.cpp app/agent_wiring.cpp tests/test_capability_policy.cpp tests/test_wiring_mcp.cpp CMakeLists.txt
git commit -m "feat: McpTool capability — mcp_allow scope (exact or *), confirm default, mcp block-name gate"
```

---

## Task 5: Ship docs — manifest-reference + CLAUDE.md

**Files:**
- Modify: `docs/manifest-reference.md` (§4 Tool blocks + capability_policy section + capability verdict table)
- Modify: `CLAUDE.md` (feature section, backlog item, test count)

**Do NOT touch `manifests/dev.hades`** (Global Constraints deviation note — the example blocks below ship in the reference doc as paste-ready snippets instead).

**Interfaces:**
- Consumes: everything shipped in T1–T4, exactly as implemented (verify claims against the code before writing them).

- [ ] **Step 1: manifest-reference §4.** Replace the `mcp` row of the key table (currently: `| \`mcp\` | MCP server command (alternative to \`native\`). | — | MCP tools are **not** announced to the LLM yet (discovery deferred). |`) with:

```markdown
| `mcp` | Local MCP server command (stdio transport, spawned per exchange). | — | Tools DISCOVERED at boot and announced as `<block>__<tool>` (see below). |
| `mcp_url` | Remote MCP server URL (Streamable HTTP transport). | — | Same discovery/announce; Bearer auth via `api_key_env`. |
| `api_key_env` | Env var holding the Bearer token for `mcp_url`. | — | Env-only (never in the manifest); absent → no auth header. Ignored for `native`/`mcp`. |
```

Then append a new subsection at the end of §4:

```markdown
### MCP servers — discovery, naming, gating

One `Tool` block = one MCP **server** (not one tool). At boot (registry warm) hades performs a
`tools/list` exchange per server and announces every discovered tool to the LLM as
**`<block>__<toolname>`** — e.g. block `weather` exposing `get_alerts` announces
`weather__get_alerts`. The prefix guarantees a server can never shadow a native tool name
(`fs_read`, `shell`, …) and inherit its capability verdict. Exactly ONE of
`native | mcp | mcp_url` per block, and an mcp block name must be `[A-Za-z0-9_-]{1,64}`
without `__` — both enforced at launch (`MalConfig`).

    # local (stdio) server — needs its runtime (node/python) on the box
    Tool = weather { mcp = npx -y @h1deya/mcp-server-weather }

    # remote (Streamable HTTP) server — token via env, never in the manifest
    Tool = linear
    {
      mcp_url     = https://mcp.linear.example/mcp
      api_key_env = LINEAR_MCP_KEY
      timeout_s   = 60
    }

**Transport.** `mcp` spawns the command per exchange (one-shot: initialize + request over
newline-delimited JSON-RPC on stdio). `mcp_url` speaks MCP Streamable HTTP: JSON-RPC over
POST, `Mcp-Session-Id` honored, responses accepted as plain JSON **or** SSE-framed, redirects
disabled, best-effort session `DELETE`. Auth is Bearer-only — a server requiring OAuth login
flows should be bridged through the stdio path instead: `Tool = x { mcp = npx -y mcp-remote
https://server.example/mcp }`.

**Gating.** Every `<block>__<tool>` maps to the `McpTool` capability: **confirm by default**
(each call needs human approval; heartbeat/peer turns auto-deny), unless listed in the
`capability_policy` scope `mcp_allow` (exact prefixed names, whitespace-separated; the single
literal `*` allows every discovered MCP tool — that trusts every rostered server, prefer
naming tools). `timeout_s` covers both the discovery exchange and each call.

**Failure + trust notes.** A server that is down (or lists nothing) at boot degrades
fail-soft: nothing announced, one stderr log line, and the legacy call-by-block-name path
remains. `mcp_url` is operator-set (not LLM-chosen), so the private-net/SSRF gate does NOT
apply to it — a loopback self-hosted server is legitimate; the gate stays on `http_fetch`.
Discovered tool descriptions/schemas are server-controlled text entering the prompt — roster
only servers you trust.
```

- [ ] **Step 2: manifest-reference capability_policy section.** In the `capability_policy` key table add:

```markdown
| `mcp_allow` | Discovered MCP tools (`<block>__<tool>`, whitespace-separated; literal `*` = all) allowed without confirm. | empty (every MCP call confirms) |
```

And in the **Capability verdict table** (the "what each tool gets" table) add:

```markdown
| `<block>__<tool>` (any MCP-discovered tool) | McpTool | in `mcp_allow` (or `*`) → allow; else **confirm** (heartbeat/peer turns auto-deny). |
```

- [ ] **Step 3: CLAUDE.md.** Three edits:

(a) In `## Other open work`, remove `MCP tool discovery (MCP servers can be called but aren't announced to the LLM) ·` from the list.

(b) Update the Current-state test count `**626/626 tests**` → `**653/653 tests**` (keep the TSan phrasing pattern: `(ASan+UBSan; TSan 614/614 as of feat/simplex — no new thread surface since)`).

(c) Add a feature subsection under Current state (after the save_skill patch-mode context if present, else after the SimpleX section):

```markdown
### MCP discovery + remote transport (shipped 2026-07-12, `feat/mcp-discovery`)
MCP servers rostered as `Tool = <block> { mcp = <cmd> }` (stdio) or `{ mcp_url = <url>
api_key_env = <ENV> }` (Streamable HTTP, Bearer-only; OAuth servers → `npx -y mcp-remote <url>`
bridge on the stdio path) get their tools DISCOVERED at registry warm (`tools/list`, one
exchange per server) and announced to the LLM as **`<block>__<tool>`** (prefix = no
native-name shadowing; `.` illegal in OpenAI-compat function names). `inputSchema` passes
1:1 into the ToolSpec; specs flow through the existing `wire_agent` → `set_tools` path (zero
Arbiter changes). tools/call sends the server's OWN name via the registry's
`mcp_real_names_` map (never string-split). **Fail-soft:** discovery failure/empty → stderr
line + legacy call-by-block-name path, boot never blocked beyond the entry timeout.
**Capability:** any `__`-containing name → `Capability::McpTool` → **confirm** by default
(heartbeat/peer auto-deny) unless listed in `capability_policy { mcp_allow = <block>__<tool>
… }` (whitespace list; literal `*` = all — trusts every rostered server). Launch gates
(`MalConfig`): exactly one of `native|mcp|mcp_url`; mcp block names `[A-Za-z0-9_-]{1,64}`
without `__`. HTTP transport: cpr POSTs, `Mcp-Session-Id` lifecycle, plain-JSON or
SSE-framed replies, redirects off, best-effort DELETE; `mcp_url` is operator-set → exempt
from the private-net gate (documented). Per-exchange one-shot (stdio spawn per call) kept —
v2 seam: warm persistent child. Docs: manifest-reference §4 MCP subsection + capability
rows. Pieces: `src/apps/tool_runner/tool_runner.cpp` (transports + discovery),
`include/hades/tool/{registry,mcp_adapter}.h`, `src/behaviors/capability_policy.cpp`
(McpTool), `app/agent_wiring.cpp` (validation + mcp_allow), `tests/test_mcp_{adapter,discovery}.cpp`,
`tests/test_wiring_mcp.cpp`, `tests/fake_mcp_server.sh`. dev.hades example NOT committed
(user's live file) — paste-ready blocks live in manifest-reference §4.
```

- [ ] **Step 4: Verify + suite.** Re-read each doc claim against the shipped code (exact key names, error behavior, defaults). Then:

```bash
nix develop --command ctest --test-dir build --output-on-failure
git status --short   # manifests/dev.hades, manifests/pi.hades, memory/facts.md MUST NOT be staged
```

Expected: **653/653**.

- [ ] **Step 5: Commit.**

```bash
git add docs/manifest-reference.md CLAUDE.md
git commit -m "docs: MCP discovery + HTTP transport — manifest-reference §4 MCP subsection, mcp_allow, CLAUDE.md"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: **653/653** (626 baseline + 27 new: 7 T1 + 6 T2 + 5 T3 + 9 T4... counts per task listed in each task's step; the authoritative number is the suite total after T4/T5).
2. TSan once at the end (spec: no new thread surface expected, but HTTP runs cpr from the pump thread like the native path):
   `nix develop --command ctest --test-dir build-tsan --output-on-failure` (rebuild build-tsan if configured; skip if the tree has no TSan config — note it in the report).
3. Manual protocol smoke (no LLM): roster a real stdio server in a THROWAWAY manifest copy (not dev.hades), boot, check stderr for discovery, `hades-scope` for the announce-driven spec list.
4. Live smoke (Vaios, later): paste the §4 example into dev.hades (his copy), boot, ask the agent "what tools do you have?" → `weather__…` listed; call one → confirm prompt; add `mcp_allow` → runs unattended.

## Execution

Subagent-driven development (house process): fresh implementer per task, per-task review, final
whole-branch review, then finishing-a-development-branch (merge ff to main — no remote, never
push). Baseline 626/626 before Task 1.
