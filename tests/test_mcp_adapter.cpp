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
