#include <gtest/gtest.h>
#include "hades/objective/stay_on_budget.h"
#include "hades/objective/avoid_destructive.h"
#include "hades/blackboard.h"
using namespace hades;
TEST(StayOnBudget, VetoesWhenOverCap) {
  Blackboard bb; StayOnBudget o(1.0);
  bb.post("BUDGET_SPENT_USD", 0.5, "llm"); bb.pump();
  EXPECT_FALSE(o.veto(bb, {Action::Kind::Answer}).vetoed);
  bb.post("BUDGET_SPENT_USD", 1.2, "llm"); bb.pump();
  EXPECT_TRUE(o.veto(bb, {Action::Kind::Answer}).vetoed);
}
TEST(AvoidDestructive, ConfirmGatesDangerousShell) {
  Blackboard bb; AvoidDestructive o;
  Action safe{Action::Kind::ToolCall}; safe.tool="fs_read"; safe.args={{"path","/a"}};
  EXPECT_FALSE(o.veto(bb,safe).vetoed);
  Action bad{Action::Kind::ToolCall}; bad.tool="shell"; bad.args={{"cmd","rm -rf /"}};
  auto v=o.veto(bb,bad);
  EXPECT_TRUE(v.vetoed); EXPECT_TRUE(v.needs_confirm);
}
