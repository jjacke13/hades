// tests/test_tools_wiring.cpp — manifest-path Tools block + the extended idle invariant
//
// With tools offloaded, a silent stretch can be a foreground tool run: boot must refuse a
// manifest whose idle ceiling doesn't outlast every foreground-effective tool timeout.
// Roster has NO llm/front-end (wiring-test precedent) — the invariant fires before any
// heavy build. Multi-line Tool blocks only (one-kv-per-line parser fails loud on packing).
#include <gtest/gtest.h>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"  // MalConfig
using namespace hades;

static Manifest mk(const std::string& extra) {
  return parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n" + extra);
}

TEST(ToolsWiring, PerToolTimeoutOverIdleCeilingThrows) {
  Blackboard bb;
  Manifest m = mk("Tool = big\n{\n  native = /bin/true\n  timeout_s = 1000\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);     // 1000 >= default idle 900
}
TEST(ToolsWiring, RunnerDefaultTimeoutOverIdleCeilingThrows) {
  Blackboard bb;
  Manifest m = mk("Tools\n{\n  timeout_s = 2000\n}\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
TEST(ToolsWiring, InRangeTimeoutsBoot) {
  Blackboard bb;
  Manifest m = mk("Tool = ok\n{\n  native = /bin/true\n  timeout_s = 600\n}\n"
                  "Tools\n{\n  background_timeout_s = 7200\n}\n");
  Agent a = build_agent(bb, m);                    // 600 < 900 boots; bg timeout is EXEMPT
  SUCCEED();
}
