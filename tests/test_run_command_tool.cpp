// tests/test_run_command_tool.cpp — drive hades-run-command over the native protocol
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
nlohmann::json run_cmd(const nlohmann::json& args) {
  ProcResult r = run_subprocess({RUN_COMMAND_BIN},
                                nlohmann::json{{"call", "run_command"}, {"args", args}}.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(RunCommandTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({RUN_COMMAND_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "run_command");
}

TEST(RunCommandTool, RunsArgvAndCapturesOutput) {
  auto j = run_cmd({{"command", "echo hello world"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("stdout", ""), "hello world\n");
  EXPECT_EQ(j["result"].value("exit_code", -1), 0);
}

TEST(RunCommandTool, NoShellSemantics) {
  // ';' is just an argv byte — no shell ever runs. echo prints it literally.
  auto j = run_cmd({{"command", "echo a;rm -rf /tmp/definitely-not-run"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("stdout", ""), "a;rm -rf /tmp/definitely-not-run\n");
  // $(...) is a literal argument, not an expansion.
  auto k = run_cmd({{"command", "echo $(whoami)"}});
  EXPECT_EQ(k["result"].value("stdout", ""), "$(whoami)\n");
}

TEST(RunCommandTool, NonzeroExitReported) {
  auto j = run_cmd({{"command", "false"}});
  ASSERT_TRUE(j.value("ok", false));               // ok = the tool ran it; exit_code carries failure
  EXPECT_NE(j["result"].value("exit_code", 0), 0);
}

TEST(RunCommandTool, MissingBinaryAndBadArgsFailClosed) {
  auto nf = run_cmd({{"command", "definitely-not-a-real-binary-xyz"}});
  ASSERT_FALSE(nf.is_discarded());
  // child exec fails -> nonzero exit code (ok stays true: the spawn contract ran); stderr explains
  EXPECT_NE(nf["result"].value("exit_code", 0), 0);
  auto empty = run_cmd({{"command", ""}});
  EXPECT_FALSE(empty.value("ok", true));
  auto typed = run_cmd({{"command", 42}});
  ASSERT_FALSE(typed.is_discarded());
  EXPECT_FALSE(typed.value("ok", true));
}

TEST(RunCommandTool, TimeoutReported) {
  auto j = run_cmd({{"command", "sleep 5"}, {"timeout_s", 0.2}});
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_TRUE(j["result"].value("timed_out", false));
}
