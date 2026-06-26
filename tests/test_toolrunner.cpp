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
