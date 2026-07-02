// tests/test_telegram_module.cpp — TelegramModule turn/confirm/allowlist logic over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/telegram_module.h"
using namespace hades;

namespace {
struct FakeApi : TelegramApi {
  std::deque<std::vector<TgUpdate>> batches;       // popped per get_updates call
  std::vector<std::pair<long long, std::string>> sent;         // send_message calls
  std::vector<std::string> confirms;                            // confirm_ids sent
  std::vector<std::string> answered;                            // callback ids answered
  bool throw_next = false;
  std::vector<TgUpdate> get_updates(long long, double) override {
    if (throw_next) { throw_next = false; throw std::runtime_error("net down"); }
    if (batches.empty()) return {};
    auto b = batches.front();
    batches.pop_front();
    return b;
  }
  bool send_message(long long chat, const std::string& t) override {
    sent.push_back({chat, t});
    return true;
  }
  bool send_confirm(long long, const std::string&, const std::string& id) override {
    confirms.push_back(id);
    return true;
  }
  void answer_callback(const std::string& id) override { answered.push_back(id); }
};

TgUpdate msg(long long uid, long long from, long long chat, const std::string& text) {
  TgUpdate u; u.update_id = uid; u.kind = "message"; u.from_id = from; u.chat_id = chat; u.text = text;
  return u;
}
TgUpdate cb(long long uid, long long from, long long chat, const std::string& cbid,
            const std::string& data) {
  TgUpdate u; u.update_id = uid; u.kind = "callback"; u.from_id = from; u.chat_id = chat;
  u.callback_id = cbid; u.callback_data = data;
  return u;
}

// Build a module over the fake api. allow_users = "42". `echo=true` installs a plain
// echo agent; tests that script their OWN bus behavior (confirm flow, long reply) pass
// false so the echo can't satisfy got_reply_ before the path under test runs.
struct Rig {
  Blackboard bb;
  FakeApi* api;
  std::unique_ptr<TelegramModule> mod;
  explicit Rig(bool echo = true) {
    auto a = std::make_unique<FakeApi>();
    api = a.get();
    mod = std::make_unique<TelegramModule>(std::move(a));
    Block cfg; cfg.kv["allow_users"] = "42";
    mod->on_start(cfg, bb);
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
    // First poll_once drains-and-discards the startup backlog (empty here).
    mod->poll_once();
  }
};
}  // namespace

TEST(TelegramModule, AllowedMessageDrivesTurnAndReplies) {
  Rig r;
  r.api->batches.push_back({msg(1, 42, -9, "hi")});
  EXPECT_TRUE(r.mod->poll_once());
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, -9);
  EXPECT_EQ(r.api->sent[0].second, "echo:hi");
}

TEST(TelegramModule, NonAllowedSenderSilentlyIgnored) {
  Rig r;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->batches.push_back({msg(1, 666, -9, "open sesame")});
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);                 // never reached the agent
  EXPECT_TRUE(r.api->sent.empty());       // and got no reply (don't reveal the bot is alive)
}

TEST(TelegramModule, StartupBacklogIsDrainedAndDiscarded) {
  Blackboard bb;
  auto a = std::make_unique<FakeApi>();
  FakeApi* api = a.get();
  auto mod = std::make_unique<TelegramModule>(std::move(a));
  Block cfg; cfg.kv["allow_users"] = "42";
  mod->on_start(cfg, bb);
  mod->on_attach(bb);
  bool user_msg = false;
  bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  // Echo responder so the fresh turn resolves at once — without an ASSISTANT_MESSAGE the
  // drive_turn_ run_until would block the full idle timeout (900s) before returning.
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
  });
  api->batches.push_back({msg(1, 42, -9, "stale command from yesterday")});
  api->batches.push_back({msg(2, 42, -9, "another stale one")});
  mod->poll_once();                        // drain pass: consumes until empty, DISCARDS all
  EXPECT_FALSE(user_msg);
  EXPECT_TRUE(api->sent.empty());
  api->batches.push_back({msg(3, 42, -9, "fresh")});
  mod->poll_once();                        // now live
  EXPECT_TRUE(user_msg);
}

TEST(TelegramModule, LongReplyIsSplitAt4096) {
  Rig r(false);                                    // own handler below, no echo
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", std::string(5000, 'x'), "t");
  });
  r.api->batches.push_back({msg(1, 42, -9, "big")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 2u);
  EXPECT_EQ(r.api->sent[0].second.size(), 4096u);
  EXPECT_EQ(r.api->sent[1].second.size(), 904u);
}

TEST(TelegramModule, ConfirmFlowApproveViaInlineKeyboard) {
  Rig r(false);                                    // scripted confirm path, no echo
  // Script: the user message gates on a confirm; approval yields the final answer.
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "run shell?"}}, "arbiter");
  });
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    if (e.value.value("approved", false))
      r.bb.post("ASSISTANT_MESSAGE", "ran it", "t");
  });
  r.api->batches.push_back({msg(1, 42, -9, "wipe build dir")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->confirms.size(), 1u);          // buttons sent
  EXPECT_EQ(r.api->confirms[0], "c1");
  EXPECT_TRUE(r.api->sent.empty());               // no reply yet — gated
  r.api->batches.push_back({cb(2, 42, -9, "cbq9", "approve:c1")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->answered.size(), 1u);          // spinner dismissed
  EXPECT_EQ(r.api->answered[0], "cbq9");
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].second, "ran it");
}

TEST(TelegramModule, StaleOrUnknownCallbackIsAnsweredButNotPosted) {
  Rig r;
  bool confirm_resp = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { confirm_resp = true; });
  r.api->batches.push_back({cb(1, 42, -9, "cbq1", "approve:ghost")});
  r.mod->poll_once();
  EXPECT_EQ(r.api->answered.size(), 1u);          // always dismiss the spinner
  EXPECT_FALSE(confirm_resp);                     // but no CONFIRM_RESPONSE for an unknown id
}

TEST(TelegramModule, ForeignTurnConfirmIsNotCaptured) {
  Rig r;
  // A confirm from a REPL/web-driven turn (module idle, my_turn_ false) must not send buttons.
  r.bb.post("CONFIRM_REQUEST", {{"id", "zz"}, {"prompt", "?"}}, "arbiter");
  r.bb.pump();
  EXPECT_TRUE(r.api->confirms.empty());
}

TEST(TelegramModule, ApiErrorSurvivesAndReportsFailure) {
  Rig r;
  r.api->throw_next = true;
  EXPECT_FALSE(r.mod->poll_once());               // error surfaced, no crash
  r.api->batches.push_back({msg(1, 42, -9, "still alive")});
  EXPECT_TRUE(r.mod->poll_once());                // next batch works
  EXPECT_EQ(r.api->sent.size(), 1u);
}

TEST(TelegramModule, MissingOrBadAllowUsersIsMalConfig) {
  Blackboard bb;
  {
    TelegramModule m(std::make_unique<FakeApi>());
    Block cfg;                                     // no allow_users at all
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
  {
    TelegramModule m(std::make_unique<FakeApi>());
    Block cfg; cfg.kv["allow_users"] = "12ab";     // non-numeric id
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
}
