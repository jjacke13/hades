// tests/test_compactor_wiring.cpp — manifest-path wiring for Module = compactor
#include <gtest/gtest.h>
#include <cstdlib>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(CompactorWiring, RosteredModuleIsBuiltAndAbsentIsNull) {
  ::setenv("HADES_API_KEY", "dummy", 1);  // on_start builds a real provider (no call made)
  Blackboard bb;
  Manifest with = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = compactor\nModule = arbiter\n"
      "Compactor\n{\n  model = cheap-model\n  summary_char_limit = 2000\n}\n");
  Agent a = build_agent(bb, with);
  EXPECT_NE(a.compactor, nullptr);
  Blackboard bb2;
  Manifest without = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent b = build_agent(bb2, without);
  EXPECT_EQ(b.compactor, nullptr);
}

TEST(CompactorWiring, MissingKeyEnvFailsLoud) {
  ::unsetenv("HADES_COMPACT_KEY_MISSING");
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = compactor\nModule = arbiter\n"
      "Compactor\n{\n  api_key_env = HADES_COMPACT_KEY_MISSING\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
