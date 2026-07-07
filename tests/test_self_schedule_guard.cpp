// tests/test_self_schedule_guard.cpp — SelfScheduleGuard veto + SelfSchedule capability
#include <gtest/gtest.h>
#include "hades/blackboard.h"
#include "hades/objective/capability_policy.h"
#include "hades/objective/self_schedule_guard.h"
using namespace hades;

static Action sched(const char* tool) {
  Action a{Action::Kind::ToolCall};
  a.tool = tool;
  a.args = {{"name", "x"}, {"prompt", "p"}, {"schedule", "* * * * *"}};
  return a;
}

TEST(SelfScheduleGuard, HeartbeatOriginVetoedWhenDisallowed) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(false);
  EXPECT_TRUE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, HeartbeatOriginAllowedWhenEnabled) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(true);
  EXPECT_FALSE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, HumanOriginAlwaysAllowed) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "human", "chat");
  SelfScheduleGuard g(false);
  EXPECT_FALSE(g.veto(bb, sched("schedule_task")).vetoed);
}

TEST(SelfScheduleGuard, OnlyGuardsCreatePath) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "heartbeat:watch", "heartbeat");
  SelfScheduleGuard g(false);
  EXPECT_FALSE(g.veto(bb, sched("list_tasks")).vetoed);
  EXPECT_FALSE(g.veto(bb, sched("cancel_task")).vetoed);
}

TEST(CapabilityPolicy, SelfScheduleToolsAreAllowed) {
  EXPECT_EQ(CapabilityPolicy::capability_of("schedule_task"), Capability::SelfSchedule);
  EXPECT_EQ(CapabilityPolicy::capability_of("list_tasks"), Capability::SelfSchedule);
  EXPECT_EQ(CapabilityPolicy::capability_of("cancel_task"), Capability::SelfSchedule);
  CapabilityScope sc;
  CapabilityPolicy p(sc);
  Blackboard bb;
  EXPECT_FALSE(p.veto(bb, sched("schedule_task")).vetoed);   // allow (guard + tool caps do the gating)
}
