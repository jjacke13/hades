// tests/test_telegram_wiring.cpp — manifest wiring for the telegram front-end
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

namespace {
std::string manifest_text(const std::string& telegram_block) {
  return std::string("Session\n{\n  model = m\n}\n") + "Module = arbiter\n" +
         "Module = telegram\n" + telegram_block;
}
}  // namespace

TEST(TelegramWiring, RosterBuildsModuleWithTokenAndAllowlist) {
  ::setenv("HADES_TEST_TG_TOKEN", "tok", 1);
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_TOKEN\n  allow_users = 42\n}\n"));
  Agent agent = build_agent(bb, m);
  EXPECT_NE(agent.telegram, nullptr);
}

TEST(TelegramWiring, MissingAllowUsersIsMalConfig) {
  ::setenv("HADES_TEST_TG_TOKEN", "tok", 1);
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_TOKEN\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TelegramWiring, MissingTokenEnvIsMalConfig) {
  ::unsetenv("HADES_TEST_TG_MISSING");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Telegram\n{\n  token_env = HADES_TEST_TG_MISSING\n  allow_users = 42\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TelegramWiring, NoTelegramRosterLeavesMemberNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.telegram, nullptr);
}
