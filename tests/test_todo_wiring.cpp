// tests/test_todo_wiring.cpp — Session.todo_file reaches BOTH the tool argv and the fold
// Roster has NO llm module (session_search-wiring precedent): the ToolRunner runs the REAL
// binary; TOOL_REQUEST driven directly, then a USER_MESSAGE proves the same-session fold.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;
namespace fs = std::filesystem;

TEST(TodoWiring, ConfiguredFileWrittenAndFoldReachesLlmRequest) {
  const std::string f =
      ::testing::TempDir() + "/todo_wire_" + std::to_string(::getpid()) + ".md";
  fs::remove(f);
  const std::string manifest =
      "Session\n{\n  model = m\n  todo_file = " + f + "\n}\n" +
      "Module = tool_runner\nModule = arbiter\n" +
      "Tool = todo { native = " + std::string(TODO_BIN) + " }\n";
  Blackboard bb;
  Manifest m = parse_manifest(manifest);
  Agent agent = build_agent(bb, m);
  bb.post("TOOL_REQUEST",
          {{"id", "t1"},
           {"tool", "todo"},
           {"args", {{"items", {{{"text", "wire the fold"}, {"status", "in_progress"}}}}}}},
          "arbiter");
  // Tool offloaded on the manifest path -> wait for the worker's TOOL_RESULT (posted only
  // after the subprocess wrote the file) before asserting the file and driving the fold turn.
  bb.run_until([&]{ auto r = bb.get("TOOL_RESULT");
                    return r.has_value() && r->value.value("id", "") == "t1"; }, 10.0);
  ASSERT_TRUE(fs::exists(f));                        // argv carried the configured path
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  const std::string sys = req->value["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("- [~] wire the fold"), std::string::npos);   // live same-session fold
}

TEST(TodoWiring, NoTodoToolMeansNoFold) {
  const std::string f =
      ::testing::TempDir() + "/todo_wire_nf_" + std::to_string(::getpid()) + ".md";
  { std::ofstream out(f); out << "- [ ] ghost\n"; }
  const std::string manifest =
      "Session\n{\n  model = m\n  todo_file = " + f + "\n}\n" +
      "Module = tool_runner\nModule = arbiter\n";
  Blackboard bb;
  Manifest m = parse_manifest(manifest);
  Agent agent = build_agent(bb, m);
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->value["messages"][0]["content"].get<std::string>().find("ghost"),
            std::string::npos);                      // fold only when the tool is rostered
}
