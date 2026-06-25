#include <gtest/gtest.h>
#include "hades/blackboard.h"
using namespace hades;
TEST(Blackboard, PostStoresLatestAndPumpDispatches) {
  Blackboard bb;
  std::vector<std::string> got;
  bb.subscribe("USER_MESSAGE", [&](const Entry& e){ got.push_back(e.value.get<std::string>()); });
  bb.post("USER_MESSAGE", "a", "chat");
  bb.post("USER_MESSAGE", "b", "chat");
  EXPECT_TRUE(got.empty());                 // nothing delivered during post
  bb.pump();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1], "b");
  EXPECT_EQ(bb.get("USER_MESSAGE")->value.get<std::string>(), "b");  // latest-value
}
TEST(Blackboard, PrefixWildcardAndStarMatch) {
  Blackboard bb; int pfx=0, all=0;
  bb.subscribe("TOOL_*", [&](const Entry&){ ++pfx; });
  bb.subscribe("*",      [&](const Entry&){ ++all; });
  bb.post("TOOL_REQUEST", 1, "arb");
  bb.post("USER_MESSAGE", "x", "chat");
  bb.pump();
  EXPECT_EQ(pfx, 1); EXPECT_EQ(all, 2);
}
TEST(Blackboard, HandlerCanPostMore) {
  Blackboard bb; int n=0;
  bb.subscribe("A", [&](const Entry&){ bb.post("B", 1, "s"); });
  bb.subscribe("B", [&](const Entry&){ ++n; });
  bb.post("A", 1, "s"); bb.pump();
  EXPECT_EQ(n, 1);
}
