// tests/test_pantler_wiring.cpp — the Module= roster drives which modules the agent builds
#include <gtest/gtest.h>
#include <memory>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"   // MalConfig
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Minimal manifest with the LLM env var satisfied is awkward (build_agent(Manifest) resolves a real
// provider). These tests use the Manifest overload, so set the api key env in the manifest to one we set.
const char* kFullRoster = R"(
Session
{
  provider       = openai_compat
  endpoint       = https://example.invalid/v1
  model          = test-model
  api_key_env    = HADES_TEST_KEY
  price_per_mtok = 1.0
}
Module = llm
Module = tool_runner
Module = memory
Module = chat
Module = arbiter
Module = serve
Memory
{
  store = .hades/test_mem.jsonl
  top_n = 5
}
Arbiter { policy = v1 }
)";
}  // namespace

TEST(PantlerWiring, FullRosterBuildsAllModules) {
  setenv("HADES_TEST_KEY", "x", 1);
  Blackboard bb;
  Agent a = build_agent(bb, parse_manifest(kFullRoster));
  EXPECT_NE(a.llm, nullptr);
  EXPECT_NE(a.tools, nullptr);
  EXPECT_NE(a.memory, nullptr);
  EXPECT_NE(a.arbiter, nullptr);
  EXPECT_NE(a.chat, nullptr);
  EXPECT_NE(a.serve, nullptr);
}
TEST(PantlerWiring, RosterOmittingServeYieldsNullServe) {
  setenv("HADES_TEST_KEY", "x", 1);
  std::string m = kFullRoster;
  // drop the serve line
  auto pos = m.find("Module = serve\n");
  ASSERT_NE(pos, std::string::npos);
  m.erase(pos, std::string("Module = serve\n").size());
  Blackboard bb;
  Agent a = build_agent(bb, parse_manifest(m));
  EXPECT_EQ(a.serve, nullptr);    // roster drives presence
  EXPECT_NE(a.arbiter, nullptr);  // the rest still built
}
TEST(PantlerWiring, UnknownModuleTypeThrows) {
  setenv("HADES_TEST_KEY", "x", 1);
  std::string m = kFullRoster;
  m += "\nModule = bogus\n";
  Blackboard bb;
  EXPECT_THROW(build_agent(bb, parse_manifest(m)), MalConfig);
}
TEST(PantlerWiring, MisorderedRosterStillBuilds) {
  setenv("HADES_TEST_KEY", "x", 1);
  const char* misordered = R"(
Session
{
  provider       = openai_compat
  endpoint       = https://example.invalid/v1
  model          = test-model
  api_key_env    = HADES_TEST_KEY
  price_per_mtok = 1.0
}
Module = arbiter
Module = llm
Module = memory
Module = tool_runner
Module = chat
Memory
{
  store = .hades/test_mem.jsonl
  top_n = 5
}
)";
  Blackboard bb;
  Agent a = build_agent(bb, parse_manifest(misordered));
  EXPECT_NE(a.arbiter, nullptr);
  EXPECT_NE(a.memory, nullptr);   // built regardless of roster order; wire_agent enforces attach order
}
TEST(PantlerWiring, CorruptInlineMultiKvManifestThrowsMalConfig) {
  setenv("HADES_TEST_KEY", "x", 1);
  // A full roster, but the Memory block packs two kv pairs on ONE line (the silent-corruption
  // footgun). enforce_manifest() must promote the parser's fatal warning to MalConfig.
  const char* corrupt = R"(
Session
{
  endpoint    = https://example.invalid/v1
  model       = test-model
  api_key_env = HADES_TEST_KEY
}
Module = llm
Module = arbiter
Memory { store=x top_n=5 }
)";
  Blackboard bb;
  EXPECT_THROW(build_agent(bb, parse_manifest(corrupt)), MalConfig);
}
// Discriminating tests for enforce_manifest itself: a packed kv that is NOT a memory
// store path (so the downstream reject_ws guard would NOT catch it) must still throw —
// proving enforce_manifest, not some other wiring guard, does the promotion.
TEST(PantlerWiring, EnforceManifestThrowsOnPackedKv) {
  Manifest m = parse_manifest("Session { endpoint=a model=b }\n");
  ASSERT_FALSE(fatal_warnings(m).empty());   // T1 detector flagged the packed line
  EXPECT_THROW(enforce_manifest(m), MalConfig);
}
TEST(PantlerWiring, EnforceManifestNoOpOnCleanManifest) {
  Manifest m = parse_manifest(kFullRoster);  // all multi-line / single-kv inline
  ASSERT_TRUE(fatal_warnings(m).empty());
  EXPECT_NO_THROW(enforce_manifest(m));
}
