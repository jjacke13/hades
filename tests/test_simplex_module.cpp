// tests/test_simplex_module.cpp — SimplexModule allowlist/turn/confirm/notify over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include <signal.h>
#include <unistd.h>
#include "hades/module/simplex_module.h"
#include "hades/stt/provider.h"
using namespace hades;

namespace {
struct FakeApi : SimplexApi {
  std::deque<SxEvent> events;                                    // popped per next_event
  std::vector<std::pair<long long, std::string>> sent;           // send_text calls
  std::vector<long long> accepted;                               // accept_request calls
  std::vector<std::pair<long long, std::string>> received;       // receive_file calls
  bool receive_file_ok = true;                                   // scriptable /freceive outcome
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
  bool receive_file(long long fid, const std::string& dest) override {
    received.push_back({fid, dest});
    return receive_file_ok;
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

SxEvent voice_ev(long long cid, const std::string& name, long long fid, long long size) {
  SxEvent e; e.kind = SxEvent::Kind::Voice; e.contact_id = cid; e.display_name = name;
  e.file_id = fid; e.file_size = size; e.duration = 3;
  return e;
}
SxEvent file_ev(SxEvent::Kind k, long long fid) {
  SxEvent e; e.kind = k; e.file_id = fid;   // FileDone/FileFailed carry no contact
  return e;
}

// Scripted STT: records the paths it was handed, returns a canned transcript (no backend needed).
struct FakeStt : SttProvider {
  std::string transcript = "hello from voice";
  bool ok = true;
  std::vector<std::string> paths;
  SttResult transcribe(const std::string& audio_path) override {
    paths.push_back(audio_path);
    SttResult r;
    r.ok = ok;
    if (ok) r.text = transcript; else r.error = "stt down";
    return r;
  }
};

void touch_file(const std::string& p) { std::ofstream f(p, std::ios::binary); f << "audio"; }
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

TEST(SimplexModule, DaemonCommandSpawnedOnStartAndReapedOnDestroy) {
  // Simplex.command: hades spawns the daemon child itself. Proven with `sleep` standing in
  // for simplex-chat: alive after start(), SIGTERM+reaped by the dtor (no orphan, no zombie).
  struct NoConnectFake : FakeApi {                 // backoff path -> event thread sleeps
    bool reconnect() override { return false; }
  };
  int pid = 0;
  {
    Blackboard bb;
    SimplexModule m(std::make_unique<NoConnectFake>());
    Block cfg;
    cfg.kv["allow_contacts"] = "1";
    cfg.kv["command"] = "sleep 1000";
    m.on_start(cfg, bb);
    m.on_attach(bb);
    m.start();
    pid = m.daemon_pid();
    ASSERT_GT(pid, 0);
    EXPECT_EQ(::kill(pid, 0), 0);                  // child alive
  }                                                // dtor: join event thread, TERM + reap child
  EXPECT_NE(::kill(pid, 0), 0);                    // gone (ESRCH; pid-reuse race negligible)
}

TEST(SimplexModule, NoCommandMeansNoDaemon) {
  Blackboard bb;
  SimplexModule m(std::make_unique<FakeApi>());
  Block cfg;
  cfg.kv["allow_contacts"] = "1";
  m.on_start(cfg, bb);
  EXPECT_EQ(m.daemon_pid(), 0);
}

// ── Voice input ───────────────────────────────────────────────────────────────────────────────
// A voice offer is only ACCEPTED (/freceive) after the allowlist + size gates; the bytes arrive
// later as FileDone, which is matched back to a contact through the pending table alone.

TEST(SimplexModuleVoice, AllowlistedVoiceIsAcceptedThenTranscribedIntoATurn) {
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  std::string user_msg;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry& e) { user_msg = e.value.get<std::string>(); });
  r.api->events.push_back(voice_ev(2, "whoever", 11, 4096));
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 1u);                 // accepted before any bytes moved
  EXPECT_EQ(r.api->received[0].first, 11);
  const std::string dest = r.api->received[0].second;
  EXPECT_NE(dest.find("hades-sx-voice-11"), std::string::npos);
  EXPECT_TRUE(r.api->sent.empty());                      // no turn yet — bytes not on disk
  touch_file(dest);                                      // the daemon "delivers" the file
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 11));
  r.pump_events();
  ASSERT_EQ(stt.paths.size(), 1u);
  EXPECT_EQ(stt.paths[0], dest);
  EXPECT_EQ(user_msg, "hello from voice");                // a normal turn, like a typed one
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 2);
  EXPECT_EQ(r.api->sent[0].second, "echo:hello from voice");
  EXPECT_FALSE(std::filesystem::exists(dest));            // temp deleted on the success path
}

TEST(SimplexModuleVoice, NonAllowlistedVoiceNeverCallsReceiveFile) {
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.api->events.push_back(voice_ev(666, "stranger", 12, 4096));
  r.pump_events();
  EXPECT_TRUE(r.api->received.empty());                   // no /freceive for a stranger
  EXPECT_TRUE(r.api->sent.empty());                       // silently dropped, as for Text
}

TEST(SimplexModuleVoice, OversizeVoiceIsRefusedWithoutReceiveFile) {
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.api->events.push_back(voice_ev(2, "V", 13, 20LL * 1024 * 1024));   // over the 10 MB default
  r.pump_events();
  EXPECT_TRUE(r.api->received.empty());
  ASSERT_EQ(r.api->sent.size(), 1u);                      // the sender is told, not ignored
  EXPECT_NE(r.api->sent[0].second.find("too large"), std::string::npos);
}

TEST(SimplexModuleVoice, NonPositiveFileSizeIsRefusedWithoutReceiveFile) {
  // An absent/mistyped/wrapped size parses to <= 0: unknown size, so the cap cannot be checked.
  for (long long size : {0LL, -1LL}) {
    Rig r;
    FakeStt stt;
    r.mod->set_stt(&stt);
    r.api->events.push_back(voice_ev(2, "V", 14, size));
    r.pump_events();
    EXPECT_TRUE(r.api->received.empty());
    ASSERT_EQ(r.api->sent.size(), 1u);
    // Refused for the TRUE reason: unknown size, not "too large" (which would send the sender
    // off shortening a message whose length was never the problem).
    EXPECT_NE(r.api->sent[0].second.find("couldn't tell how big"), std::string::npos);
    EXPECT_EQ(r.api->sent[0].second.find("too large"), std::string::npos);
  }
}

TEST(SimplexModuleVoice, NoSttProviderMeansNoReceiveFile) {
  Rig r;                                                  // set_stt never called
  r.api->events.push_back(voice_ev(2, "V", 15, 4096));
  r.pump_events();
  EXPECT_TRUE(r.api->received.empty());
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("aren't enabled"), std::string::npos);
}

TEST(SimplexModuleVoice, TranscribeFailureRepliesAndPostsNoUserMessage) {
  Rig r;
  FakeStt stt;
  stt.ok = false;
  r.mod->set_stt(&stt);
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->events.push_back(voice_ev(2, "V", 16, 4096));
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 1u);
  const std::string dest = r.api->received[0].second;
  touch_file(dest);
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 16));
  r.pump_events();
  EXPECT_FALSE(user_msg);                                 // no turn on a failed transcribe
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("didn't catch that"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dest));            // temp deleted on the failure path too
}

TEST(SimplexModuleVoice, FileDoneForUnknownFileIdIsIgnored) {
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 999));   // never accepted by us
  r.api->events.push_back(file_ev(SxEvent::Kind::FileFailed, 998));
  r.pump_events();
  EXPECT_TRUE(stt.paths.empty());
  EXPECT_TRUE(r.api->sent.empty());                       // no contact to reply to — silent
}

TEST(SimplexModuleVoice, FileFailedDropsThePendingEntry) {
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.api->events.push_back(voice_ev(2, "V", 17, 4096));
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 1u);
  const std::string dest = r.api->received[0].second;
  touch_file(dest);
  r.api->events.push_back(file_ev(SxEvent::Kind::FileFailed, 17));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("didn't come through"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dest));            // partial temp reclaimed
  // Entry is gone: a late completion for the same id is now a stranger.
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 17));
  r.pump_events();
  EXPECT_TRUE(stt.paths.empty());
  EXPECT_EQ(r.api->sent.size(), 1u);
}

TEST(SimplexModuleVoice, PendingTableIsCappedAndEvictsOldest) {
  // Offers that never complete must not grow the table: inserting the 9th drops the oldest
  // entry AND its temp file, so a never-completing sender cannot leak state or disk.
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  for (long long fid = 101; fid <= 108; ++fid)            // fill to kMaxPendingVoice (8)
    r.api->events.push_back(voice_ev(2, "V", fid, 4096));
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 8u);
  const std::string evicted = r.api->received[0].second;  // fileId 101, the oldest
  touch_file(evicted);
  r.api->events.push_back(voice_ev(2, "V", 109, 4096));   // the 9th evicts it
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 9u);
  EXPECT_FALSE(std::filesystem::exists(evicted));         // its temp file went with it
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 101));   // evicted -> ignored
  r.pump_events();
  EXPECT_TRUE(stt.paths.empty());
  EXPECT_TRUE(r.api->sent.empty());
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 109));   // still pending -> works
  r.pump_events();
  ASSERT_EQ(stt.paths.size(), 1u);
  EXPECT_EQ(r.api->sent.size(), 1u);
}

TEST(SimplexModuleVoice, ReceiveFileFailureRepliesAndLeavesNoPendingEntry) {
  // /freceive refused (or its reply timed out): the sender is told, no entry is stored, and a
  // FileDone that arrives anyway — the daemon may have taken the request — is a stranger. The
  // file that daemon may still write is reclaimed by the per-pid temp dir, not by an entry.
  Rig r;
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.api->receive_file_ok = false;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->events.push_back(voice_ev(2, "V", 21, 4096));
  r.pump_events();
  ASSERT_EQ(r.api->received.size(), 1u);                  // we did ask
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_NE(r.api->sent[0].second.find("couldn't download"), std::string::npos);
  r.api->events.push_back(file_ev(SxEvent::Kind::FileDone, 21));
  r.pump_events();
  EXPECT_TRUE(stt.paths.empty());                         // no entry -> nothing transcribed
  EXPECT_FALSE(user_msg);
  EXPECT_EQ(r.api->sent.size(), 1u);                      // and no second reply
}

TEST(SimplexModuleVoice, VoiceDuringAnOutstandingConfirmIsRefusedAndLeavesItArmed) {
  // Approval is a security boundary: a transcript must never answer a y/N (a mishearing would
  // become approved:true on a confirm-gated action). Refuse, keep the confirm armed for TEXT.
  Rig r(false);
  FakeStt stt;
  r.mod->set_stt(&stt);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c9"}, {"prompt", "sure?"}}, "arbiter");
  });
  bool confirm_answered = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) {
    confirm_answered = true;
    r.bb.post("ASSISTANT_MESSAGE", "done", "t");
  });
  r.api->events.push_back(text_ev(2, "V", "risky"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);                      // the y/N prompt
  r.api->events.push_back(voice_ev(2, "V", 31, 4096));
  r.pump_events();
  EXPECT_TRUE(r.api->received.empty());                   // nothing accepted off the daemon
  EXPECT_FALSE(confirm_answered);                         // the confirm was NOT consumed
  ASSERT_EQ(r.api->sent.size(), 2u);
  EXPECT_NE(r.api->sent[1].second.find("y/n"), std::string::npos);
  r.api->events.push_back(text_ev(2, "V", "y"));          // still armed: TEXT answers it
  r.pump_events();
  EXPECT_TRUE(confirm_answered);
}

TEST(SimplexModuleVoice, DestructorRemovesThePerProcessVoiceTempDir) {
  // Shutdown reclaim: a transfer still in flight (or a /freceive the daemon took after we gave
  // up on it) leaves audio no pending entry points at. The per-pid dir is what takes it.
  struct NoConnectFake : FakeApi {                        // parks the event thread in backoff
    bool reconnect() override { return false; }
  };
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("hades-sx-voice-" + std::to_string(::getpid()));
  {
    Blackboard bb;
    SimplexModule m(std::make_unique<NoConnectFake>());
    Block cfg;
    cfg.kv["allow_contacts"] = "1";
    m.on_start(cfg, bb);
    m.on_attach(bb);
    m.start();                                            // creates the dir before the thread
    ASSERT_TRUE(std::filesystem::is_directory(dir));
    touch_file((dir / "hades-sx-voice-42.bin").string()); // an in-flight transfer's partial file
  }                                                       // dtor: join, reap, remove_all
  EXPECT_FALSE(std::filesystem::exists(dir));
}
