// tests/test_mcp_adapter.cpp — mcp_list/mcp_call over both transports (stdio here; http in T2)
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <thread>
#include <httplib.h>
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
