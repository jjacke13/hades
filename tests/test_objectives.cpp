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
TEST(StayOnBudget, VetoesExactlyAtCap) {
  Blackboard bb; StayOnBudget o(1.0);
  bb.post("BUDGET_SPENT_USD", 1.0, "llm"); bb.pump();
  EXPECT_TRUE(o.veto(bb, {Action::Kind::Answer}).vetoed);
}
TEST(StayOnBudget, NoKeyDefaultsToZeroNoVeto) {
  Blackboard bb; StayOnBudget o(1.0);
  EXPECT_FALSE(o.veto(bb, {Action::Kind::Answer}).vetoed);
}
TEST(StayOnBudget, NonNumericBudgetDoesNotThrow) {
  Blackboard bb; StayOnBudget o(1.0);
  bb.post("BUDGET_SPENT_USD", "oops", "x"); bb.pump();
  EXPECT_NO_THROW({ auto v=o.veto(bb,{Action::Kind::Answer}); EXPECT_FALSE(v.vetoed); });
}
TEST(AvoidDestructive, IgnoresNonToolCallWithDangerousText) {
  Blackboard bb; AvoidDestructive o;
  Action a{Action::Kind::Answer}; a.text="i will rm -rf / everything";
  EXPECT_FALSE(o.veto(bb,a).vetoed);
}
TEST(AvoidDestructive, CatchesMkfs) {
  Blackboard bb; AvoidDestructive o;
  Action a{Action::Kind::ToolCall}; a.tool="shell"; a.args={{"cmd","mkfs.ext4 /dev/sda"}};
  EXPECT_TRUE(o.veto(bb,a).vetoed);
}
