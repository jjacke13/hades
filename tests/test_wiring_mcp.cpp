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
