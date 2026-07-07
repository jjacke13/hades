// tests/test_telegram_module.cpp — TelegramModule turn/confirm/allowlist logic over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/telegram_module.h"
#include "hades/stt/provider.h"
#include "hades/tts/provider.h"
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
  std::string file_path_ret;                     // get_file_path returns this
  std::string download_ret;                      // download_file returns this
  std::string get_file_path(const std::string&) override { return file_path_ret; }
  std::string download_file(const std::string&) override { return download_ret; }
  std::vector<std::pair<long long, std::string>> voices;   // send_voice(chat, bytes) calls
  bool voice_fails = false;
  bool send_voice(long long chat, const std::string& bytes) override {
    if (voice_fails) return false;
    voices.push_back({chat, bytes});
    return true;
  }
};

struct FakeStt : SttProvider {
  SttResult ret;
  std::string last_path;
  SttResult transcribe(const std::string& audio_path) override {
    last_path = audio_path;
    return ret;
  }
};

struct FakeTts : TtsProvider {
  TtsResult ret;
  std::string last_text;
  TtsResult synthesize(const std::string& text) override { last_text = text; return ret; }
};

TgUpdate voice(long long uid, long long from, long long chat, const std::string& fid) {
  TgUpdate u; u.update_id = uid; u.kind = "message"; u.from_id = from; u.chat_id = chat;
  u.voice_file_id = fid;
  return u;
}

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
  r.api->batches.push_back({msg(1, 42, 42, "hi")});   // DM: chat_id == from_id
  EXPECT_TRUE(r.mod->poll_once());
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 42);
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

TEST(TelegramModule, GroupChatMessageIgnoredEvenFromAllowedUser) {
  Rig r;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->batches.push_back({msg(1, 42, -100999, "hi from a group")});   // chat_id != from_id
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);
  EXPECT_TRUE(r.api->sent.empty());   // no reply into the group
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
  api->batches.push_back({msg(3, 42, 42, "fresh")});   // DM: chat_id == from_id
  mod->poll_once();                        // now live
  EXPECT_TRUE(user_msg);
}

TEST(TelegramModule, LongReplyIsSplitAt4096) {
  Rig r(false);                                    // own handler below, no echo
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", std::string(5000, 'x'), "t");
  });
  r.api->batches.push_back({msg(1, 42, 42, "big")});   // DM: chat_id == from_id
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
  r.api->batches.push_back({msg(1, 42, 42, "wipe build dir")});   // DM: chat_id == from_id
  r.mod->poll_once();
  ASSERT_EQ(r.api->confirms.size(), 1u);          // buttons sent
  EXPECT_EQ(r.api->confirms[0], "c1");
  EXPECT_TRUE(r.api->sent.empty());               // no reply yet — gated
  r.api->batches.push_back({cb(2, 42, 42, "cbq9", "approve:c1")});
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
  r.api->batches.push_back({msg(1, 42, 42, "still alive")});   // DM: chat_id == from_id
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

TEST(TelegramModule, VoiceMessageTranscribesAndDrivesTurn) {
  Rig r;                                     // echo agent installed
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello from voice", "", ""};
  FakeStt* sp = stt.get();
  r.mod->set_stt(sp);
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "OGGbytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  EXPECT_TRUE(r.mod->poll_once());
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].second, "echo:hello from voice");   // transcript drove the turn
  EXPECT_FALSE(sp->last_path.empty());                          // a temp file path was passed
}

TEST(TelegramModule, VoiceWithNoSttProviderNudges) {
  Rig r(false);                              // no echo; provider not set
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);                                       // no turn
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("isn't enabled"), std::string::npos);
}

TEST(TelegramModule, VoiceTranscriptionFailureRepliesNoTurn) {
  Rig r(false);
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {false, "", "", "backend down"};
  r.mod->set_stt(stt.get());
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("couldn't transcribe"), std::string::npos);
}

TEST(TelegramModule, VoiceEmptyTranscriptSaysDidntCatch) {
  Rig r(false);
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "   ", "", ""};          // whitespace-only -> treated as empty
  r.mod->set_stt(stt.get());
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  EXPECT_FALSE(user_msg);
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("didn't catch that"), std::string::npos);
}

TEST(TelegramModule, VoiceDownloadFailureReplies) {
  Rig r(false);
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "unused", "", ""};
  r.mod->set_stt(stt.get());
  r.api->file_path_ret = "";                 // getFile fails -> empty path
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("couldn't"), std::string::npos);
}

TEST(TelegramModule, VoiceTurnSpeaksReplyAfterText) {
  Rig r;                                   // echo agent: reply = "echo:<transcript>"
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello", "", ""};
  r.mod->set_stt(stt.get());
  auto tts = std::make_unique<FakeTts>();
  tts->ret = {true, "OGGDATA", ""};
  FakeTts* tp = tts.get();
  r.mod->set_tts(tp);
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].second, "echo:hello");           // text sent (anchor)
  ASSERT_EQ(r.api->voices.size(), 1u);                       // AND spoken
  EXPECT_EQ(r.api->voices[0].second, "OGGDATA");
  EXPECT_EQ(tp->last_text, "echo:hello");                    // synthesized the reply text
}

TEST(TelegramModule, TypedTurnDoesNotSpeak) {
  Rig r;
  auto tts = std::make_unique<FakeTts>();
  tts->ret = {true, "OGGDATA", ""};
  FakeTts* tp = tts.get();
  r.mod->set_tts(tp);
  r.api->batches.push_back({msg(1, 42, 42, "hi")});           // typed
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);                          // text reply
  EXPECT_TRUE(r.api->voices.empty());                         // NO voice
  EXPECT_TRUE(tp->last_text.empty());                         // synthesize never called
}

TEST(TelegramModule, TtsFailureLeavesTextOnly) {
  Rig r;
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello", "", ""};
  r.mod->set_stt(stt.get());
  auto tts = std::make_unique<FakeTts>();
  tts->ret = {false, "", "backend down"};
  r.mod->set_tts(tts.get());
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);                          // text still sent
  EXPECT_TRUE(r.api->voices.empty());                         // no voice on synth failure
}

TEST(TelegramModule, LongReplySkipsTts) {
  Rig r(false);   // no echo; we post our own long reply
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello", "", ""};
  r.mod->set_stt(stt.get());
  auto tts = std::make_unique<FakeTts>();
  tts->ret = {true, "OGGDATA", ""};
  FakeTts* tp = tts.get();
  r.mod->set_tts(tp);
  r.mod->set_tts_max_chars(5);                                // tiny cap
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", "this reply is far too long", "t");
  });
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);                          // text sent
  EXPECT_TRUE(r.api->voices.empty());                         // over cap -> no TTS
  EXPECT_TRUE(tp->last_text.empty());                         // synthesize not called
}

TEST(TelegramModule, VoiceTurnWithNoTtsProviderStaysText) {
  Rig r;
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello", "", ""};
  r.mod->set_stt(stt.get());                                  // STT set, TTS not
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_TRUE(r.api->voices.empty());                         // tts_ null -> text only
}

TEST(TelegramModule, NotifyUserSendsToAllowedUsers) {
  // A heartbeat (or anything) posts NOTIFY_USER -> pushed to EVERY allowed user. Not my_turn_-gated;
  // fires on a plain post+pump with no turn. allow_ is a std::set<long long> -> iterated sorted.
  Blackboard bb;
  auto a = std::make_unique<FakeApi>();
  FakeApi* api = a.get();
  auto mod = std::make_unique<TelegramModule>(std::move(a));
  Block cfg; cfg.kv["allow_users"] = "111 222";
  mod->on_start(cfg, bb);
  mod->on_attach(bb);
  bb.post("NOTIFY_USER", {{"text", "pi0 down"}, {"from", "heartbeat:mon"}}, "heartbeat");
  bb.pump();
  ASSERT_EQ(api->sent.size(), 2u);              // one per allowed user
  EXPECT_EQ(api->sent[0].first, 111);
  EXPECT_EQ(api->sent[0].second, "pi0 down");
  EXPECT_EQ(api->sent[1].first, 222);
  EXPECT_EQ(api->sent[1].second, "pi0 down");
}

TEST(TelegramModule, NotifyUserEmptyTextSendsNothing) {
  Blackboard bb;
  auto a = std::make_unique<FakeApi>();
  FakeApi* api = a.get();
  auto mod = std::make_unique<TelegramModule>(std::move(a));
  Block cfg; cfg.kv["allow_users"] = "111 222";
  mod->on_start(cfg, bb);
  mod->on_attach(bb);
  bb.post("NOTIFY_USER", {{"from", "heartbeat:mon"}}, "heartbeat");   // object, no "text"
  bb.pump();
  EXPECT_TRUE(api->sent.empty());
}

TEST(TelegramModule, SpeakFlagDoesNotLeakVoiceToNextTypedTurn) {
  // Regression: a voice turn sets speak_reply_; the per-turn RAII Reset must clear it so a
  // FOLLOWING typed turn in the same batch does NOT get spoken. Both turns in one poll batch.
  Rig r;                                   // echo agent
  auto stt = std::make_unique<FakeStt>();
  stt->ret = {true, "hello", "", ""};
  r.mod->set_stt(stt.get());
  auto tts = std::make_unique<FakeTts>();
  tts->ret = {true, "OGGDATA", ""};
  r.mod->set_tts(tts.get());
  r.api->file_path_ret = "voice/f.oga";
  r.api->download_ret = "bytes";
  r.api->batches.push_back({voice(1, 42, 42, "AwAC"), msg(2, 42, 42, "typed")});
  r.mod->poll_once();
  ASSERT_EQ(r.api->sent.size(), 2u);                          // both replies as text
  EXPECT_EQ(r.api->sent[0].second, "echo:hello");            // voice turn
  EXPECT_EQ(r.api->sent[1].second, "echo:typed");            // typed turn
  ASSERT_EQ(r.api->voices.size(), 1u);                        // ONLY the voice turn spoke
  EXPECT_EQ(r.api->voices[0].second, "OGGDATA");
}
