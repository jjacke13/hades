// tests/test_auto_extract_wiring.cpp — manifest-path wiring for Module = auto_extract
#include <gtest/gtest.h>
#include <cstdlib>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(AutoExtractWiring, RosteredModuleIsBuiltAndAbsentIsNull) {
  ::setenv("HADES_API_KEY", "dummy", 1);  // on_start builds a real provider (no call is made)
  Blackboard bb;
  Manifest with = parse_manifest(
      "Session\n{\n  model = m\n  endpoint = http://127.0.0.1:9\n}\n"
      "Module = auto_extract\nModule = arbiter\n"
      "AutoExtract\n{\n  model = cheap-model\n  max_facts = 2\n}\n");
  Agent a = build_agent(bb, with);
  EXPECT_NE(a.auto_extract, nullptr);
  Blackboard bb2;
  Manifest without = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent b = build_agent(bb2, without);
  EXPECT_EQ(b.auto_extract, nullptr);
}
