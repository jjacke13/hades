// tests/test_heartbeat_wiring.cpp — manifest-path wiring for the heartbeat module
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(HeartbeatWiring, ParsesEntriesInlineAndFile) {
  const std::string pf = std::string(::testing::TempDir()) + "/hb_prompt.txt";
  { std::ofstream f(pf); f << "summarize the day"; }
  const std::string mtext =
      "Session\n{\n  model = m\n}\n"
      "Module = arbiter\nModule = heartbeat\n"
      "Heartbeat = mon\n{\n  schedule = */10 * * * *\n  prompt = check pi0\n  notify = true\n}\n"
      "Heartbeat = daily\n{\n  schedule = 0 6 * * *\n  prompt_file = " + pf + "\n  notify = false\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.heartbeat, nullptr);
  // No timer thread is started by build_agent (only hades_main calls start()).
}

TEST(HeartbeatWiring, BadCronThrowsMalConfig) {
  const std::string mtext =
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = heartbeat\n"
      "Heartbeat = bad\n{\n  schedule = not a cron\n  prompt = x\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(HeartbeatWiring, MissingPromptThrowsMalConfig) {
  const std::string mtext =
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = heartbeat\n"
      "Heartbeat = np\n{\n  schedule = * * * * *\n  notify = true\n}\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(HeartbeatWiring, NoHeartbeatRosterLeavesNull) {
  const std::string mtext = "Session\n{\n  model = m\n}\nModule = arbiter\n";
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.heartbeat, nullptr);
}
