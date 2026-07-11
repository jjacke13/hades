// tests/test_simplex_module.cpp — SimplexModule allowlist/turn/confirm/notify over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/simplex_module.h"
using namespace hades;

namespace {
struct FakeApi : SimplexApi {
  std::deque<SxEvent> events;                                    // popped per next_event
  std::vector<std::pair<long long, std::string>> sent;           // send_text calls
  std::vector<long long> accepted;                               // accept_request calls
  int reconnects = 0;
  // Empty script -> Closed (NOT Timeout) so the Rig's pump_events loop terminates; it also
  // exercises the loop-ends-on-Closed contract on every test.
  SxStatus next_event(double, SxEvent& out) override {
    if (events.empty()) return SxStatus::Closed;
    out = events.front();
    events.pop_front();
    return SxStatus::Event;
  }
  bool send_text(long long cid, const std::string& t) override {
    sent.push_back({cid, t});
    return true;
  }
  bool accept_request(long long rid) override {
    accepted.push_back(rid);
    return true;
  }
  bool reconnect() override { ++reconnects; return true; }
};

SxEvent text_ev(long long cid, const std::string& name, const std::string& text) {
  SxEvent e; e.kind = SxEvent::Kind::Text; e.contact_id = cid; e.display_name = name; e.text = text;
  return e;
}
SxEvent request_ev(long long rid, const std::string& name) {
  SxEvent e; e.kind = SxEvent::Kind::ContactRequest; e.request_id = rid; e.display_name = name;
  return e;
}
SxEvent connected_ev(long long cid, const std::string& name) {
  SxEvent e; e.kind = SxEvent::Kind::Connected; e.contact_id = cid; e.display_name = name;
  return e;
}

// Rig over the fake api. allow_contacts = "2, Vaios K". echo=true installs a plain echo agent.
struct Rig {
  Blackboard bb;
  FakeApi* api;
  std::unique_ptr<SimplexModule> mod;
  explicit Rig(bool echo = true, const std::string& extra_key = "", const std::string& extra_val = "") {
    auto a = std::make_unique<FakeApi>();
    api = a.get();
    mod = std::make_unique<SimplexModule>(std::move(a));
    Block cfg;
    cfg.kv["allow_contacts"] = "2, Vaios K";
    if (!extra_key.empty()) cfg.kv[extra_key] = extra_val;
    mod->on_start(cfg, bb);
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
  }
  void pump_events() { while (mod->step_once()) {} }   // drains the script; ends on Closed
};
}  // namespace

TEST(SimplexModule, MissingOrEmptyAllowContactsThrows) {
  Blackboard bb;
  {
    SimplexModule m(std::make_unique<FakeApi>());
    Block cfg;                                        // no allow_contacts at all
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
  {
    SimplexModule m(std::make_unique<FakeApi>());
    Block cfg;
    cfg.kv["allow_contacts"] = "  ,  ";               // empty after split/trim
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
}

TEST(SimplexModule, AllowedIdDrivesTurnAndReplies) {
  Rig r;
  r.api->events.push_back(text_ev(2, "whoever", "hi"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 2);
  EXPECT_EQ(r.api->sent[0].second, "echo:hi");
}

TEST(SimplexModule, AllowedDisplayNameDrivesTurn) {
  Rig r;
  r.api->events.push_back(text_ev(77, "Vaios K", "name match"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 77);                // reply goes to the sender's id
}

TEST(SimplexModule, NonAllowedSenderSilentlyDropped) {
  Rig r;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->events.push_back(text_ev(666, "stranger", "let me in"));
  r.pump_events();
  EXPECT_FALSE(user_msg);
  EXPECT_TRUE(r.api->sent.empty());
}

TEST(SimplexModule, LongReplySplitAt4000) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", std::string(4001, 'z'), "t");
  });
  r.api->events.push_back(text_ev(2, "V", "long please"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 2u);
  EXPECT_EQ(r.api->sent[0].second.size(), 4000u);
  EXPECT_EQ(r.api->sent[1].second.size(), 1u);
}

TEST(SimplexModule, ConfirmYesApprovesViaTextReply) {
  Rig r(false);
  // Turn 1: the agent asks for confirmation. Turn 2 (the CONFIRM_RESPONSE): replies.
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "rm -rf /tmp/x — sure?"}}, "arbiter");
  });
  nlohmann::json confirm_resp;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_resp = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "done", "t");
  });
  r.api->events.push_back(text_ev(2, "V", "do the risky thing"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);                              // the y/N prompt text
  EXPECT_NE(r.api->sent[0].second.find("rm -rf /tmp/x"), std::string::npos);
  EXPECT_NE(r.api->sent[0].second.find("reply y"), std::string::npos);
  r.api->events.push_back(text_ev(2, "V", "  Y  "));              // case+space tolerant approve
  r.pump_events();
  ASSERT_TRUE(confirm_resp.is_object());
  EXPECT_EQ(confirm_resp.value("id", ""), "c1");
  EXPECT_TRUE(confirm_resp.value("approved", false));
  EXPECT_EQ(r.api->sent.back().second, "done");
}

TEST(SimplexModule, ConfirmAnythingElseDenies) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c2"}, {"prompt", "sure?"}}, "arbiter");
  });
  nlohmann::json confirm_resp;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_resp = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "declined then", "t");
  });
  r.api->events.push_back(text_ev(2, "V", "risky"));
  r.pump_events();
  r.api->events.push_back(text_ev(2, "V", "actually never mind"));
  r.pump_events();
  ASSERT_TRUE(confirm_resp.is_object());
  EXPECT_FALSE(confirm_resp.value("approved", true));
}

TEST(SimplexModule, ConfirmAnswerMustComeFromSameContact) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    // Only the FIRST user message triggers a confirm; the other contact's text is a normal turn.
    static bool first = true;
    if (first) {
      first = false;
      r.bb.post("CONFIRM_REQUEST", {{"id", "c3"}, {"prompt", "sure?"}}, "arbiter");
    } else {
      r.bb.post("ASSISTANT_MESSAGE", "other turn: " + e.value.get<std::string>(), "t");
    }
  });
  bool confirm_answered = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { confirm_answered = true; });
  r.api->events.push_back(text_ev(2, "V", "risky"));
  r.pump_events();
  // A DIFFERENT allowlisted contact (by name) speaks: must NOT be consumed as the confirm answer.
  r.api->events.push_back(text_ev(77, "Vaios K", "unrelated"));
  r.pump_events();
  EXPECT_FALSE(confirm_answered);
  EXPECT_EQ(r.api->sent.back().second, "other turn: unrelated");
}

TEST(SimplexModule, AutoAcceptOffLogsAndIgnoresOnAccepts) {
  {
    Rig r;                                                        // default auto_accept=false
    r.api->events.push_back(request_ev(9, "stranger"));
    r.pump_events();
    EXPECT_TRUE(r.api->accepted.empty());
  }
  {
    Rig r(true, "auto_accept", "true");
    r.api->events.push_back(request_ev(9, "stranger"));
    r.pump_events();
    ASSERT_EQ(r.api->accepted.size(), 1u);
    EXPECT_EQ(r.api->accepted[0], 9);
  }
}

TEST(SimplexModule, NotifyIsQueuedOnPumpAndDeliveredOnTheEventThread) {
  // The C1 contract: the NOTIFY_USER subscriber (running on whatever thread pumps the post —
  // the heartbeat timer in production) must NOT send; delivery happens when the event loop
  // drains the queue in step_once. Single socket, single owning thread.
  Rig r(true, "notify_contact", "2");
  r.bb.post("NOTIFY_USER", {{"text", "heartbeat says hi"}, {"from", "heartbeat"}}, "hb");
  r.bb.pump();
  EXPECT_TRUE(r.api->sent.empty());              // queued only — no send on the pump thread
  r.pump_events();                                // event thread drains
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 2);
  EXPECT_EQ(r.api->sent[0].second, "heartbeat says hi");
}

TEST(SimplexModule, NotifyByNameResolvesViaKnownContacts) {
  Rig r(true, "notify_contact", "Vaios K");
  // Name not yet resolvable when drained -> delivery skipped (logged), no crash.
  r.bb.post("NOTIFY_USER", {{"text", "too early"}}, "hb");
  r.bb.pump();
  r.pump_events();
  EXPECT_TRUE(r.api->sent.empty());
  // A Connected event teaches the name->id mapping; the next notify delivers on the drain.
  r.api->events.push_back(connected_ev(42, "Vaios K"));
  r.pump_events();
  r.bb.post("NOTIFY_USER", {{"text", "now it works"}}, "hb");
  r.bb.pump();
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 42);
}

TEST(SimplexModule, ClosedYieldsFalseForTheReconnectPath) {
  Rig r;
  EXPECT_FALSE(r.mod->step_once());        // empty script = Closed -> false (loop reconnects)
}
