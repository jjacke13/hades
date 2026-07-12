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

TEST(McpDiscovery, ProviderIllegalToolNamesSkipped) {
  // A discovered name with chars outside [A-Za-z0-9_-] (or empty) would 400 the WHOLE tools
  // array at the LLM API — it must be skipped while the server's clean tools survive.
  ::setenv("FAKE_MCP_LIST_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"tools":[)"
           R"({"name":"bad/name","description":"x"},{"name":"has space","description":"y"},)"
           R"({"name":"clean_tool","description":"z","inputSchema":{"type":"object"}}]}})", 1);
  ToolRegistry reg;
  reg.add_from_block(mcp_block("srv", FAKE_MCP_SERVER));
  auto specs = reg.specs(10.0);
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].name, "srv__clean_tool");
}

TEST(McpDiscovery, DuplicateToolNamesDedupedFirstWins) {
  // A buggy/hostile server listing the same tool name twice must NOT announce a duplicate
  // function name (providers reject a tools array with duplicates -> the whole turn 400s).
  // First-wins across specs_, routing, and the real-name map alike.
  ::setenv("FAKE_MCP_LIST_REPLY",
           R"({"jsonrpc":"2.0","id":2,"result":{"tools":[)"
           R"({"name":"dup","description":"first","inputSchema":{"type":"object"}},)"
           R"({"name":"dup","description":"second"}]}})", 1);
  ToolRegistry reg;
  reg.add_from_block(mcp_block("srv", FAKE_MCP_SERVER));
  auto specs = reg.specs(10.0);
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].name, "srv__dup");
  EXPECT_EQ(specs[0].description, "first");
  EXPECT_EQ(reg.mcp_real_name("srv__dup"), "dup");
}
