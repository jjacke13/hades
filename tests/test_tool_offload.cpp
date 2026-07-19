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

TEST(BackgroundTools, TruncUtf8Bytes) {
  EXPECT_EQ(trunc_utf8_bytes("short", 10), "short");
  std::string s(5, 'a'); s += "\xCE\xB1";                    // 5 ascii + 2-byte U+03B1 = 7 bytes
  EXPECT_EQ(trunc_utf8_bytes(s, 6), std::string(5, 'a') + " (truncated)");  // walks back over the split codepoint
  EXPECT_EQ(trunc_utf8_bytes(s, 7), s);                      // exact fit: untouched
  EXPECT_EQ(trunc_utf8_bytes(std::string(10, 'b'), 4), "bbbb (truncated)");
}

TEST(BackgroundTools, SchemaGainsBackgroundProperty) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("d", 0.0);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  auto specs = tr.registry().specs();
  ASSERT_EQ(specs.size(), 1u);
  ASSERT_TRUE(specs[0].schema["properties"].contains("background"));
  EXPECT_EQ(specs[0].schema["properties"]["background"]["type"], "boolean");
}

TEST(BackgroundTools, StartedResultThenBgDoneThenBgTasks) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("e", 0.2);
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  std::string tasks;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.subscribe("BG_TASKS", [&](const Entry& e) { if (e.value.is_string()) tasks = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", {{"background", true}}}, {"epoch", 3}},
          "arbiter");
  bb.pump();                                       // the started-result is IMMEDIATE
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_TRUE(result["content"].value("started", false));
  const std::string tid = result["content"].value("task_id", "");
  EXPECT_EQ(tid.rfind("bg-", 0), 0u);
  EXPECT_EQ(result.value("epoch", 0), 3);
  EXPECT_NE(tasks.find(tid + " · slow · running"), std::string::npos);
  ASSERT_TRUE(bb.run_until(
      [&] { return tasks.find("finished ok") != std::string::npos; }, 5.0));
  EXPECT_NE(tasks.find("slow done"), std::string::npos);     // real output in the block
}

TEST(BackgroundTools, ToolNeverSeesBackgroundArgAndNoExecutorFallsBackInline) {
  // Reflective tool: reports LEAKED if the background flag reaches its stdin.
  const std::string p =
      ::testing::TempDir() + "/echo_args_" + std::to_string(::getpid()) + ".sh";
  {
    std::ofstream f(p);
    f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
      << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"echoargs\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
      << "*background*) echo '{\"ok\":false,\"result\":{\"error\":\"LEAKED\"}}' ;;\n"
      << "*) echo '{\"ok\":true,\"result\":{\"clean\":true}}' ;;\nesac\n";
  }
  ::chmod(p.c_str(), 0755);
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "echoargs"; b.kv["native"] = p;
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  // NO executor: the background flag is ignored (inline foreground) but still STRIPPED.
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "echoargs"}, {"args", {{"background", true}, {"x", 1}}}},
          "arbiter");
  bb.pump();
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_TRUE(result["content"].value("clean", false));      // real output, not {started}
}

TEST(BackgroundTools, CapRefusesOverMaxBackground) {
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("f", 1.0);
  tr.add_tool(b);
  Block cfg; cfg.kv["max_background"] = "1";
  tr.on_start(cfg, bb);
  tr.on_attach(bb);
  std::vector<nlohmann::json> results;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { results.push_back(e.value); });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", {{"background", true}}}}, "arbiter");
  bb.post("TOOL_REQUEST",
          {{"id", "t2"}, {"tool", "slow"}, {"args", {{"background", true}}}}, "arbiter");
  bb.pump();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_TRUE(results[0]["content"].value("started", false));
  EXPECT_FALSE(results[1].value("ok", true));                // cap 1: second refused
  EXPECT_NE(results[1]["content"].value("error", "").find("too many background"),
            std::string::npos);
  // Teardown note: ex (declared last) joins the 1s worker; its BG_DONE posts to the
  // still-alive bb and is simply never pumped — that's fine.
}

TEST(BackgroundTools, FinishedRingKeepsLastFiveAndTruncatesOutput) {
  // Big-output tool: ~3000-char result field -> the finished entry must carry the marker.
  const std::string p =
      ::testing::TempDir() + "/big_out_" + std::to_string(::getpid()) + ".sh";
  {
    std::ofstream f(p);
    f << "#!/bin/sh\nread line\ncase \"$line\" in\n"
      << "*describe*) echo '{\"ok\":true,\"result\":{\"name\":\"big\",\"description\":\"d\",\"schema\":{}}}' ;;\n"
      << "*) printf '{\"ok\":true,\"result\":{\"big\":\"'\n"
      << "   i=0; while [ $i -lt 300 ]; do printf 'ABCDEFGHIJ'; i=$((i+1)); done\n"
      << "   printf '\"}}\\n' ;;\nesac\n";
  }
  ::chmod(p.c_str(), 0755);
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "big"; b.kv["native"] = p;
  tr.add_tool(b);
  tr.on_start(Block{}, bb);
  tr.on_attach(bb);
  std::string tasks;
  bb.subscribe("BG_TASKS", [&](const Entry& e) { if (e.value.is_string()) tasks = e.value; });
  Executor ex(2);
  tr.set_executor(&ex);
  for (int i = 0; i < 6; ++i) {                    // sequential: never trips the cap
    bb.post("TOOL_REQUEST",
            {{"id", "t" + std::to_string(i)}, {"tool", "big"},
             {"args", {{"background", true}}}},
            "arbiter");
    const std::string want = "bg-" + std::to_string(i) + " · big · finished ok";
    ASSERT_TRUE(bb.run_until([&] { return tasks.find(want) != std::string::npos; }, 5.0))
        << "task " << i;
  }
  EXPECT_EQ(tasks.find("bg-0 ·"), std::string::npos);        // ring cap 5: oldest dropped
  EXPECT_NE(tasks.find("bg-5 ·"), std::string::npos);
  EXPECT_NE(tasks.find(" (truncated)"), std::string::npos);  // 3000-char output capped at 2000B
}

TEST(BackgroundTools, ThrowingWorkerStillDrainsRunningSlot) {
  // Args with invalid UTF-8 make call.dump() throw inside execute_tool on the worker; the
  // submit wrapper must convert the throw into a terminal BG_DONE{ok:false} so the running
  // slot always drains — else the cap wedges permanently (review I1).
  Blackboard bb;
  ToolRunner tr;
  Block b; b.section = "Tool"; b.name = "slow"; b.kv["native"] = write_slow_tool("g", 0.0);
  tr.add_tool(b);
  Block cfg; cfg.kv["max_background"] = "1";
  tr.on_start(cfg, bb);
  tr.on_attach(bb);
  std::string tasks;
  std::vector<nlohmann::json> results;
  bb.subscribe("BG_TASKS", [&](const Entry& e) { if (e.value.is_string()) tasks = e.value; });
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { results.push_back(e.value); });
  Executor ex(2);
  tr.set_executor(&ex);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"}, {"tool", "slow"}, {"args", {{"background", true}, {"bad", "\xFF\xFE"}}}},
          "arbiter");
  ASSERT_TRUE(bb.run_until([&] { return tasks.find("FAILED") != std::string::npos; }, 5.0));
  EXPECT_NE(tasks.find("tool threw"), std::string::npos);
  // The slot drained: a second background task under cap 1 must START, not be refused.
  bb.post("TOOL_REQUEST",
          {{"id", "t2"}, {"tool", "slow"}, {"args", {{"background", true}}}}, "arbiter");
  ASSERT_TRUE(bb.run_until([&] { return results.size() >= 2; }, 5.0));
  EXPECT_TRUE(results.back()["content"].value("started", false)) << results.back().dump();
}
