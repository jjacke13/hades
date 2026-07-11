// tests/test_simplex_wiring.cpp — manifest roster -> SimplexModule wired with gate + config
#include <gtest/gtest.h>
#include <memory>
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
