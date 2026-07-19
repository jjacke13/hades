// tests/test_tool_offload.cpp — ToolRunner Executor offload: the bus stays live mid-tool
//
// Writes a self-contained slow native tool (shell script) at runtime — no CMake fixture —
// and proves: (1) an offloaded tool completes a request via run_until; (2) the bus
// dispatches OTHER traffic while the tool is still running; (3) the request's epoch is
// echoed into TOOL_RESULT; (4) no executor -> inline synchronous (legacy shape, no epoch
// key when the request had none). The Executor is declared AFTER the modules in each test
// -> destroyed FIRST -> joins its worker while ToolRunner + Blackboard are alive (the
// load-bearing live teardown order; offload_e2e precedent).
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/executor.h"
#include "hades/module/tool_runner.h"
using namespace hades;

namespace {
// A native tool answering describe immediately and sleeping before answering a call.
std::string write_slow_tool(const char* tag, double sleep_s) {
  const std::string p = ::testing::TempDir() + "/slow_tool_" + tag + "_" +
                        std::to_string(::getpid()) + ".sh";
  std::ofstream f(p);
  f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
    << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"slow\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
    << "*) sleep " << sleep_s << "\n"
    << "   echo '{\"ok\":true,\"result\":{\"note\":\"slow done\"}}' ;;\nesac\n";
  f.close();
  ::chmod(p.c_str(), 0755);
  return p;
}
}  // namespace

TEST(ToolOffload, OffloadedToolCompletesViaRunUntilAndEchoesEpoch) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("a", 0.2);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  Executor ex(2);                        // declared last: destroyed first, joins the worker
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}, {"epoch", 7}},
          "arbiter");
  ASSERT_TRUE(bb.run_until([&] { return !result.is_null(); }, 5.0));
  EXPECT_TRUE(result.value("ok", false));
  EXPECT_EQ(result["content"].value("note", ""), "slow done");
  EXPECT_EQ(result.value("epoch", 0), 7);          // echoed through the worker
}

TEST(ToolOffload, BusDispatchesOtherTrafficWhileToolRuns) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("b", 0.5);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  bool side = false;
  nlohmann::json result;
  bb.subscribe("SIDE_CHANNEL", [&](const Entry&) { side = true; });
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}}, "arbiter");
  bb.post("SIDE_CHANNEL", true, "test");
  // The side post dispatches on the FIRST pump inside run_until — long before the 0.5s
  // tool finishes. Pre-offload, the inline handler would have blocked pump() until done.
  ASSERT_TRUE(bb.run_until([&] { return side; }, 5.0));
  EXPECT_TRUE(result.is_null());                   // tool still running (0.5s margin)
  ASSERT_TRUE(bb.run_until([&] { return !result.is_null(); }, 5.0));
  EXPECT_TRUE(result.value("ok", false));
}

TEST(ToolOffload, NoExecutorRunsInlineSynchronously) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("c", 0.0);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", nlohmann::json::object()}}, "arbiter");
  bb.pump();                                       // no executor: ONE pump completes it
  EXPECT_TRUE(result.value("ok", false));
  EXPECT_FALSE(result.contains("epoch"));          // absent in -> absent out (legacy shape)
}
