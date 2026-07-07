// tests/test_heartbeat_module.cpp — HeartbeatModule: tick() fires gated self-turns, notify, guards
#include <gtest/gtest.h>
#include <ctime>
#include <mutex>
#include <string>
#include "hades/blackboard.h"
#include "hades/module/heartbeat_module.h"
#include "hades/turn_gate.h"
using namespace hades;

namespace {
std::tm at(int min, int hour) {   // a wall-clock minute; other fields are wildcarded in tests
  std::tm t{};
  t.tm_min = min; t.tm_hour = hour; t.tm_mday = 15; t.tm_mon = 5; t.tm_wday = 3;
  t.tm_year = 126; t.tm_yday = 165;
  return t;
}
// Rig: a HeartbeatModule wired to a bus with a scripted "agent" that echoes USER_MESSAGE ->
// ASSISTANT_MESSAGE, so run_until resolves without a real LLM.
struct Rig {
  Blackboard bb;
  TurnGate gate;
  HeartbeatModule mod;
  std::string reply = "report: all good";     // what the scripted agent answers
  Rig() {
    mod.set_turn_gate(&gate);
    mod.on_attach(bb);
    bb.subscribe("USER_MESSAGE",
                 [this](const Entry&) { bb.post("ASSISTANT_MESSAGE", reply, "test"); });
  }
};
}  // namespace

TEST(Heartbeat, FiresMatchingEntryOncePerMinute) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("TURN_ORIGIN", [&](const Entry& e) {
    if (e.value.is_string() && e.value.get<std::string>() == "heartbeat:mon") ++turns;
  });
  r.mod.add_entry({"mon", "*/10 * * * *", "check", false, -1});
  r.mod.tick(at(10, 4));   // matches */10
  r.mod.tick(at(10, 4));   // SAME minute -> dedup, no second fire
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(11, 4));   // non-matching minute
  EXPECT_EQ(turns, 1);
  r.mod.tick(at(20, 4));   // next matching minute -> fires again
  EXPECT_EQ(turns, 2);
}

TEST(Heartbeat, NotifyFalseDropsReply) {
  Rig r;
  bool notified = false;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry&) { notified = true; });
  r.mod.add_entry({"task", "* * * * *", "do work", false, -1});
  r.mod.tick(at(0, 0));
  EXPECT_FALSE(notified);   // notify=false -> reply dropped
}

TEST(Heartbeat, NotifyTrueForwardsNonSilentReply) {
  Rig r;
  r.reply = "pi0 is DOWN";
  std::string got;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry& e) { got = e.value.value("text", ""); });
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  EXPECT_EQ(got, "pi0 is DOWN");
}

TEST(Heartbeat, NotifyTrueSilentSentinelIsDropped) {
  Rig r;
  r.reply = "SILENT";
  bool notified = false;
  r.bb.subscribe("NOTIFY_USER", [&](const Entry&) { notified = true; });
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  EXPECT_FALSE(notified);
}

TEST(Heartbeat, SkipsWhenTurnGateBusy) {
  Rig r;
  bool fired = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { fired = true; });
  bool skipped = false;
  r.bb.subscribe("HEARTBEAT_SKIPPED", [&](const Entry&) { skipped = true; });
  std::lock_guard<std::mutex> hold(r.gate.mu);   // a "human turn" holds the gate
  r.mod.add_entry({"mon", "* * * * *", "check", true, -1});
  r.mod.tick(at(0, 0));
  r.bb.pump();
  EXPECT_FALSE(fired);
  EXPECT_TRUE(skipped);
}

TEST(Heartbeat, ConfirmBandAutoDenied) {
  Blackboard bb;
  TurnGate gate;
  HeartbeatModule mod;
  mod.set_turn_gate(&gate);
  mod.on_attach(bb);
  nlohmann::json resp;
  // Scripted agent: on the tick, raise a confirm; when it's answered, finish the turn.
  bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "rm?"}}, "arbiter");
  });
  bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    resp = e.value;
    bb.post("ASSISTANT_MESSAGE", "[declined]", "arbiter");
  });
  mod.add_entry({"mon", "* * * * *", "risky", false, -1});
  mod.tick(at(0, 0));
  ASSERT_TRUE(resp.is_object());
  EXPECT_EQ(resp.value("id", ""), "c1");
  EXPECT_FALSE(resp.value("approved", true));
}

TEST(Heartbeat, NoEntryNoFire) {
  Rig r;
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 0);
}
