// tests/test_toolrunner.cpp — unit tests for ToolRunner and ToolRegistry on the Blackboard
//
// Verifies that ToolRunner subscribes TOOL_REQUEST, dispatches to the native
// fs_read subprocess by reported tool name (via ToolRegistry::find_by_tool_name),
// and posts TOOL_RESULT; also covers unknown-tool and missing-file error paths
// that Arbiter must handle when deciding next actions.

#include <gtest/gtest.h>
#include <fstream>
#include "hades/module/tool_runner.h"
#include "hades/blackboard.h"
using namespace hades;

TEST(ToolRunner, RunsNativeFsReadAndPostsResult) {
  Blackboard bb; ToolRunner tr;
  // Block name is "fs"; the tool self-reports "fs_read" via `describe`.
  // The request below names the *reported* tool name, exercising the
  // describe-name routing (find_by_tool_name).
  Block t; t.section="Tool"; t.name="fs"; t.kv["native"]=FS_READ_BIN;
  tr.add_tool(t); tr.on_start({}, bb); tr.on_attach(bb);
  std::string path=testing::TempDir()+"/r.txt"; { std::ofstream f(path); f<<"DATA"; }
  bool ok=false; std::string content;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e){
    ok=e.value["ok"]; content=e.value["content"].value("content","");
  });
  bb.post("TOOL_REQUEST", {{"id","1"},{"tool","fs_read"},{"args",{{"path",path}}}}, "arb");
  bb.pump();
  EXPECT_TRUE(ok); EXPECT_EQ(content,"DATA");
}

TEST(ToolRegistry, DescribeYieldsSpec) {
  ToolRegistry reg; Block t; t.name="fs"; t.kv["native"]=FS_READ_BIN; reg.add_from_block(t);
  auto specs=reg.specs();
  ASSERT_EQ(specs.size(),1u);
  EXPECT_EQ(specs[0].name,"fs_read");
}

TEST(ToolRunner, UnknownToolPostsError) {
  Blackboard bb; ToolRunner tr;
  Block t; t.section="Tool"; t.name="fs"; t.kv["native"]=FS_READ_BIN;
  tr.add_tool(t); tr.on_start({}, bb); tr.on_attach(bb);
  bool ok=true; std::string err;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e){
    ok=e.value["ok"]; err=e.value["content"].value("error","");
  });
  bb.post("TOOL_REQUEST", {{"id","9"},{"tool","does_not_exist"},{"args",nlohmann::json::object()}}, "arb");
  bb.pump();
  EXPECT_FALSE(ok);
  EXPECT_NE(err.find("unknown tool"), std::string::npos);
}

TEST(ToolRunner, NativeFsReadMissingFileReportsNotOk) {
  Blackboard bb; ToolRunner tr;
  Block t; t.section="Tool"; t.name="fs"; t.kv["native"]=FS_READ_BIN;
  tr.add_tool(t); tr.on_start({}, bb); tr.on_attach(bb);
  bool ok=true; std::string err;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e){
    ok=e.value["ok"]; err=e.value["content"].value("error","");
  });
  bb.post("TOOL_REQUEST",
          {{"id","2"},{"tool","fs_read"},{"args",{{"path","/nonexistent/hades/nope.txt"}}}},
          "arb");
  bb.pump();
  EXPECT_FALSE(ok);
  EXPECT_NE(err.find("cannot open"), std::string::npos);
}

TEST(ToolRegistry, PerToolTimeoutParsedFromBlock) {
  ToolRegistry reg;
  Block b;
  b.name = "slow_tool";
  b.kv["native"] = "/bin/true";
  b.kv["timeout_s"] = "190";
  reg.add_from_block(b);
  ASSERT_EQ(reg.entries().size(), 1u);
  EXPECT_DOUBLE_EQ(reg.entries()[0].timeout_s, 190.0);
  Block d;
  d.name = "default_tool";
  d.kv["native"] = "/bin/true";
  reg.add_from_block(d);
  EXPECT_DOUBLE_EQ(reg.entries()[1].timeout_s, 0.0);   // 0 -> runner default
}
