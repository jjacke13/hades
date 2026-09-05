// tests/test_simplex_wiring.cpp — manifest roster -> SimplexModule wired with gate + config,
// plus the voice-input seams (Stt provider injection + the voice_max_bytes cap).
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(SimplexWiring, RosterBuildsModuleAndRequiresAllowContacts) {
  {
    Blackboard bb;
    Manifest m = parse_manifest(
        "Session\n{\n  model = m\n}\nModule = arbiter\nModule = simplex\n"
        "Simplex\n{\n  allow_contacts = 2, Vaios K\n}\n");
    Agent agent = build_agent(bb, m);
    ASSERT_NE(agent.simplex, nullptr);
  }
  {
    Blackboard bb;
    Manifest m = parse_manifest(
        "Session\n{\n  model = m\n}\nModule = arbiter\nModule = simplex\n");
    EXPECT_THROW(build_agent(bb, m), MalConfig);   // no Simplex block -> no allow_contacts
  }
}

TEST(SimplexWiring, NoRosterLeavesAgentSimplexNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.simplex, nullptr);
}

// --- voice input: the Stt block is the opt-in switch, voice_max_bytes the size cap -----------
static const char* kSimplexRoster =
    "Session\n{\n  model = m\n}\nModule = arbiter\nModule = simplex\n"
    "Simplex\n{\n  allow_contacts = 2, Vaios K\n";   // caller closes the block

TEST(SimplexWiring, SttProviderIsInjectedWhenTheSttBlockIsPresent) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string(kSimplexRoster) + "}\n" +
      "Stt\n{\n  provider = command\n  command = ./tools/whisper_reference.sh\n}\n");
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.simplex, nullptr);
  ASSERT_NE(agent.stt, nullptr);
  EXPECT_EQ(agent.simplex->stt(), agent.stt.get());
}

TEST(SimplexWiring, NoSttBlockLeavesSimplexTextOnly) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kSimplexRoster) + "}\n");
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.simplex, nullptr);
  EXPECT_EQ(agent.stt, nullptr);
  EXPECT_EQ(agent.simplex->stt(), nullptr);
}

TEST(SimplexWiring, VoiceMaxBytesIsParsedFromTheSimplexBlock) {
  Blackboard bb;
  Manifest m = parse_manifest(std::string(kSimplexRoster) + "  voice_max_bytes = 4096\n}\n");
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.simplex, nullptr);
  EXPECT_EQ(agent.simplex->voice_max_bytes(), 4096);
}

TEST(SimplexWiring, GarbageVoiceMaxBytesFallsBackToTheDefault) {
  for (const char* bad : {"lots", "0", "-1"}) {
    Blackboard bb;
    Manifest m = parse_manifest(std::string(kSimplexRoster) +
                                "  voice_max_bytes = " + bad + "\n}\n");
    Agent agent = build_agent(bb, m);
    ASSERT_NE(agent.simplex, nullptr);
    EXPECT_EQ(agent.simplex->voice_max_bytes(), 10485760) << "input: " << bad;
  }
}
