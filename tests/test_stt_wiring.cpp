// tests/test_stt_wiring.cpp — manifest-path STT wiring: the Stt block builds a provider (or throws).
// Roster = tool_runner + arbiter (no llm needed — same no-provider wiring-test precedent as the
// skills/embedding wiring tests). These assert provider construction, the MalConfig fail-loud paths,
// and the no-block null path. End-to-end voice flow is covered in the TelegramModule tests (T3).
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

static const char* kRoster = "Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n";

TEST(SttWiring, HttpBlockBuildsProvider) {
  ::setenv("HADES_API_KEY", "k", 1);
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string(kRoster) +
      "Stt\n{\n  provider = http\n  endpoint = https://api.ppq.ai/v1\n  model = whisper-1\n}\n");
  Agent a = build_agent(bb, m);
  EXPECT_NE(a.stt, nullptr);
}

TEST(SttWiring, CommandBlockBuildsProvider) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string(kRoster) +
      "Stt\n{\n  provider = command\n  command = ./tools/whisper_reference.sh\n}\n");
  Agent a = build_agent(bb, m);
  EXPECT_NE(a.stt, nullptr);
}

TEST(SttWiring, NoBlockLeavesSttNull) {
  Blackboard bb;
  Manifest m = parse_manifest(kRoster);
  Agent a = build_agent(bb, m);
  EXPECT_EQ(a.stt, nullptr);
}

TEST(SttWiring, HttpWithoutEndpointThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Stt\n{\n  provider = http\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(SttWiring, CommandWithoutCommandThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Stt\n{\n  provider = command\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(SttWiring, UnknownProviderThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Stt\n{\n  provider = bogus\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
