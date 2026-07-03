// tests/test_peer_loop_guard.cpp — PeerLoopGuard: no onward ask_agent in a peer-driven turn
#include <gtest/gtest.h>
#include "hades/blackboard.h"
#include "hades/objective/peer_loop_guard.h"
using namespace hades;

namespace {
Action ask_action() {
  Action a;
  a.kind = Action::Kind::ToolCall;
  a.tool = "ask_agent";
  a.args = {{"peer", "front"}, {"message", "hi"}};
  return a;
}
}  // namespace

TEST(PeerLoopGuard, VetoesAskAgentInPeerTurn) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  PeerLoopGuard g;
  auto v = g.veto(bb, ask_action());
  EXPECT_TRUE(v.vetoed);
  EXPECT_FALSE(v.needs_confirm);                 // hard veto, not confirm
}

TEST(PeerLoopGuard, AllowsAskAgentInHumanTurn) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "human", "chat");
  PeerLoopGuard g;
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);
}

TEST(PeerLoopGuard, AllowsWhenOriginAbsentOrMalformed) {
  Blackboard bb;
  PeerLoopGuard g;
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);     // no TURN_ORIGIN at all
  bb.post("TURN_ORIGIN", 42, "x");                   // non-string
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);
}

TEST(PeerLoopGuard, IgnoresOtherToolsAndAnswers) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  PeerLoopGuard g;
  Action fs; fs.kind = Action::Kind::ToolCall; fs.tool = "fs_read";
  EXPECT_FALSE(g.veto(bb, fs).vetoed);           // only ask_agent is loop-relevant
  Action ans; ans.kind = Action::Kind::Answer;
  EXPECT_FALSE(g.veto(bb, ans).vetoed);
}
