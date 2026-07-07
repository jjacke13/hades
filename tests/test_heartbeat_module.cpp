// tests/test_heartbeat_module.cpp — HeartbeatModule: tick() fires gated self-turns, notify, guards
#include <gtest/gtest.h>
#include <unistd.h>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include "hades/blackboard.h"
#include "hades/heartbeat/cron_store.h"
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

// A human confirm from ANOTHER front-end frees the gate but leaves the Arbiter's pending_ set (async
// confirm). A tick must NOT fire into that window (else it clobbers the human's pending confirm).
TEST(Heartbeat, SkipsWhenConfirmOutstanding) {
  Rig r;
  int user_msgs = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++user_msgs; });
  bool skipped = false;
  r.bb.subscribe("HEARTBEAT_SKIPPED", [&](const Entry&) { skipped = true; });
  r.mod.add_entry({"mon", "* * * * *", "check", false, -1});

  // Human confirm from another surface: posted OUTSIDE a tick, so my_turn_ is false.
  r.bb.post("CONFIRM_REQUEST", {{"id", "h1"}, {"prompt", "approve?"}}, "arbiter");
  r.bb.pump();

  r.mod.tick(at(0, 0));   // gate is free, but a confirm is outstanding -> skip
  r.bb.pump();
  EXPECT_TRUE(skipped);
  EXPECT_EQ(user_msgs, 0);   // the tick posted no USER_MESSAGE

  // Human answers -> confirm resolved -> the next tick may fire.
  r.bb.post("CONFIRM_RESPONSE", {{"id", "h1"}, {"approved", true}}, "arbiter");
  r.bb.pump();
  r.mod.tick(at(1, 0));   // next minute
  EXPECT_EQ(user_msgs, 1);
}

// A dangling confirm from an ABANDONED turn must be cleared, else the heartbeat skips forever.
TEST(Heartbeat, ConfirmOutstandingClearedOnAbandon) {
  Rig r;
  int user_msgs = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++user_msgs; });
  r.mod.add_entry({"mon", "* * * * *", "check", false, -1});

  r.bb.post("CONFIRM_REQUEST", {{"id", "h1"}, {"prompt", "?"}}, "arbiter");
  r.bb.pump();
  r.bb.post("TURN_ABANDONED", nlohmann::json::object(), "arbiter");
  r.bb.pump();

  r.mod.tick(at(0, 0));   // dangling confirm cleared -> fires
  EXPECT_EQ(user_msgs, 1);
}

// A notify=true tick that auto-denies a confirm-band action surfaces a note in the notified reply.
TEST(Heartbeat, DeniedConfirmNoteInNotifiedReply) {
  Blackboard bb;
  TurnGate gate;
  HeartbeatModule mod;
  mod.set_turn_gate(&gate);
  mod.on_attach(bb);
  std::string got;
  bb.subscribe("NOTIFY_USER", [&](const Entry& e) { got = e.value.value("text", ""); });
  // Scripted agent: raise a confirm during the tick, then reply a non-silent answer.
  bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "rm?"}}, "arbiter");
  });
  bb.subscribe("CONFIRM_RESPONSE",
               [&](const Entry&) { bb.post("ASSISTANT_MESSAGE", "did the safe part", "arbiter"); });
  mod.add_entry({"mon", "* * * * *", "task", true, -1});   // notify=true
  mod.tick(at(0, 0));
  EXPECT_NE(got.find("auto-denied"), std::string::npos);
  EXPECT_NE(got.find("did the safe part"), std::string::npos);   // reply preserved
}

namespace {
std::string cron_store_path(const char* tag) {
  const std::string p = (std::filesystem::path(::testing::TempDir()) /
                         ("hbstore_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  std::filesystem::remove(p);
  return p;
}
void write_store(const std::string& path, const std::string& contents) {
  std::ofstream f(path, std::ios::trunc); f << contents;
}
long long local_epoch(const std::tm& t) { std::tm c = t; return static_cast<long long>(std::mktime(&c)); }
}  // namespace

TEST(Heartbeat, DynamicCronEntryFires) {
  Rig r;
  const std::string store = cron_store_path("dyncron");
  write_store(store, add_record({"d1", "watch", "cron", "*/10 * * * *", 0, "check X", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("TURN_ORIGIN", [&](const Entry& e) {
    if (e.value.is_string() && e.value.get<std::string>() == "heartbeat:watch") ++turns;
  });
  r.mod.tick(at(10, 4));   // */10 matches -> reloaded dynamic entry fires
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, OneShotFiresOnceThenDoneRecorded) {
  Rig r;
  const std::string store = cron_store_path("once");
  std::tm now = at(30, 9);
  long long past = local_epoch(now) - 60;   // due one minute ago
  write_store(store, add_record({"o1", "remind", "once", "", past, "ping", false, 5}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(now);                 // now_epoch >= fire_epoch -> fires
  EXPECT_EQ(turns, 1);
  // a done record was appended -> the task folds away
  std::ifstream f(store); std::stringstream s; s << f.rdbuf();
  EXPECT_TRUE(fold_cron_store(s.str()).empty());
  r.mod.tick(at(31, 9));           // next minute, reloads -> gone -> no re-fire
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, DynamicDedupSameMinuteAcrossReload) {
  Rig r;
  const std::string store = cron_store_path("dedup");
  write_store(store, add_record({"d1", "w", "cron", "* * * * *", 0, "c", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  r.mod.tick(at(0, 0));   // same minute, reloads again -> last_fired_by_id carries -> no double fire
  EXPECT_EQ(turns, 1);
}

TEST(Heartbeat, StaticAndDynamicCoexist) {
  Rig r;
  const std::string store = cron_store_path("coexist");
  write_store(store, add_record({"d1", "dyn", "cron", "* * * * *", 0, "c", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  r.mod.add_entry({"stat", "* * * * *", "s", false, -1});   // static entry (5-field init still valid)
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(at(0, 0));
  EXPECT_EQ(turns, 2);   // both fired
}

TEST(Heartbeat, OverdueOneShotCatchUpFires) {
  Rig r;
  const std::string store = cron_store_path("overdue");
  std::tm now = at(0, 12);
  long long long_ago = local_epoch(now) - 3 * 24 * 3600;   // 3 days overdue
  write_store(store, add_record({"o1", "late", "once", "", long_ago, "still do it", false, 1}) + "\n");
  r.mod.set_cron_store(store);
  int turns = 0;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { ++turns; });
  r.mod.tick(now);
  EXPECT_EQ(turns, 1);   // catch-up fires, not dropped
}
