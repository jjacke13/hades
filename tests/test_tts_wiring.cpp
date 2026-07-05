// tests/test_tts_wiring.cpp — manifest-path TTS wiring: the Tts block builds a provider (or throws).
// Roster = tool_runner + arbiter (no llm needed — same no-provider wiring-test precedent as STT).
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

static const char* kRoster = "Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n";

TEST(TtsWiring, HttpBlockBuildsProvider) {
  ::setenv("HADES_API_KEY", "k", 1);
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string(kRoster) +
      "Tts\n{\n  provider = http\n  endpoint = https://api.openai.com/v1\n  model = tts-1\n  voice = alloy\n}\n");
  Agent a = build_agent(bb, m);
  EXPECT_NE(a.tts, nullptr);
}

TEST(TtsWiring, CommandBlockBuildsProvider) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string(kRoster) + "Tts\n{\n  provider = command\n  command = ./tools/piper_reference.sh\n}\n");
  Agent a = build_agent(bb, m);
  EXPECT_NE(a.tts, nullptr);
}

TEST(TtsWiring, NoBlockLeavesTtsNull) {
  Blackboard bb;
  Manifest m = parse_manifest(kRoster);
  Agent a = build_agent(bb, m);
  EXPECT_EQ(a.tts, nullptr);
}

TEST(TtsWiring, HttpWithoutEndpointThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Tts\n{\n  provider = http\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TtsWiring, CommandWithoutCommandThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Tts\n{\n  provider = command\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(TtsWiring, UnknownProviderThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kRoster) + "Tts\n{\n  provider = bogus\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
