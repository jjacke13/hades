// tests/test_session_search_wiring.cpp — wiring pins the sessions dir into argv
//
// BEHAVIOUR CHANGE (daily sessions): the live-session filename is no longer appended to argv —
// pinned at launch, it could not follow the daily rollover (it would skip yesterday and rank
// today's live file). Only the DIR is pinned here, so the LLM cannot redirect the search root;
// the exclusion is injected per call by the Arbiter as the `exclude_session` arg (covered by
// Arbiter.SessionSearchExclusion* in test_arbiter.cpp).
// Roster has NO llm module: the ToolRunner runs the REAL binary; we drive TOOL_REQUEST directly.
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

TEST(SessionSearchWiring, ArgvPinsTheSessionsDir) {
  const std::string dir =
      ::testing::TempDir() + "/ss_wire_" + std::to_string(::getpid());
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto write_session = [&](const std::string& name, const std::string& u, const std::string& a) {
    std::ofstream f(dir + "/" + name);
    f << nlohmann::json{{"role", "user"}, {"content", u}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", a}}.dump() << "\n";
  };
  write_session("past.jsonl", "the zeta needle fact", "stored");
  write_session("live.jsonl", "the zeta needle fact", "live copy");
  const std::string manifest =
      "Session\n{\n  model = m\n  sessions_dir = " + dir + "\n}\n" +
      "Module = tool_runner\nModule = arbiter\n" +
      "Tool = session_search { native = " + std::string(SESSION_SEARCH_BIN) + " }\n";
  Blackboard bb;
  Manifest m = parse_manifest(manifest);
  Agent agent = build_agent(bb, m, dir + "/live.jsonl");   // live session path threaded
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  // Posted straight at the ToolRunner (no Arbiter turn), so `exclude_session` is supplied here the
  // way dispatch_or_gate would. What this test pins is the DIR: only the wiring can say where the
  // corpus lives, and both sessions are found under it.
  bb.post("TOOL_REQUEST",
          {{"id", "s1"},
           {"tool", "session_search"},
           {"args", {{"query", "zeta needle"}, {"exclude_session", "live.jsonl"}}}},
          "arbiter");
  bb.run_until([&]{ return result.is_object(); }, 10.0);   // tool offloaded on the manifest path
  ASSERT_TRUE(result.is_object());
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  const auto& hits = result["content"]["hits"];
  ASSERT_EQ(hits.size(), 1u) << result.dump();             // dir pinned; live.jsonl excluded by arg
  EXPECT_EQ(hits[0].value("session", ""), "past");
}
