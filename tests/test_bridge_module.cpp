// tests/test_bridge_module.cpp — BridgeModule inbound: auth, allowlist, ask turns, shares
#include <gtest/gtest.h>
#include <cpr/cpr.h>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "hades/blackboard.h"
#include "hades/bridge/http.h"
#include "hades/bridge/protocol.h"
#include "hades/bridge/registry.h"   // kShareTypeCard/kShareTypeFact/kCardKey (typed routing)
#include "hades/executor.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/bridge_module.h"
using namespace hades;

namespace {
// Module named "worker1", secret "s3cret", peer allowlist {front}. `echo=true` installs a
// plain echo agent; tests that script their OWN bus behavior pass false.
struct Rig {
  Blackboard bb;
  std::unique_ptr<BridgeModule> mod;
  explicit Rig(bool echo = true) {
    mod = std::make_unique<BridgeModule>("s3cret");
    Block cfg;
    cfg.kv["name"] = "worker1";
    cfg.kv["port"] = "0";                         // ephemeral -> parallel test runs never collide
    mod->on_start(cfg, bb);
    mod->set_peers({{"front", "http://127.0.0.1:1"}});
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
  }
};
std::string ask_body(const std::string& from, long long hops, const std::string& msg) {
  return build_ask(from, hops, msg).dump();
}
}  // namespace

TEST(BridgeModule, MissingNameThrowsMalConfig) {
  Blackboard bb;
  BridgeModule m("s");
  EXPECT_THROW(m.on_start(Block{}, bb), MalConfig);
  Block bad; bad.kv["name"] = "not a name";      // whitespace fails valid_peer_name
  EXPECT_THROW(m.on_start(bad, bb), MalConfig);
}

TEST(BridgeModule, UnsetSecretEnvThrowsMalConfig) {
  Blackboard bb;
  BridgeModule m;                                 // no injected secret -> resolves env
  Block cfg;
  cfg.kv["name"] = "worker1";
  cfg.kv["secret_env"] = "HADES_TEST_BRIDGE_SECRET_UNSET_XYZ";
  EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
}

TEST(BridgeModule, AskDrivesTurnAndReturnsReply) {
  Rig r;
  auto res = r.mod->handle_ask(ask_body("front", 0, "hello"), "s3cret");
  ASSERT_TRUE(res.value("ok", false)) << res.dump();
  EXPECT_EQ(res.value("reply", ""), "echo:(from peer agent \"front\") hello");
}

TEST(BridgeModule, AskPostsPeerTurnOrigin) {
  Rig r(false);
  std::string origin;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    auto o = r.bb.get("TURN_ORIGIN");
    origin = (o && o->value.is_string()) ? o->value.get<std::string>() : "<missing>";
    r.bb.post("ASSISTANT_MESSAGE", "ok", "t");
  });
  ASSERT_TRUE(r.mod->handle_ask(ask_body("front", 0, "hi"), "s3cret").value("ok", false));
  EXPECT_EQ(origin, "peer:front");   // posted BEFORE USER_MESSAGE (latest-value visible)
}

TEST(BridgeModule, BadSecretOrUnknownPeerIsForbidden) {
  Rig r;
  bool reached = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { reached = true; });
  auto bad_secret = r.mod->handle_ask(ask_body("front", 0, "hi"), "wrong");
  EXPECT_FALSE(bad_secret.value("ok", true));
  EXPECT_EQ(bad_secret.value("error", ""), "forbidden");
  auto unknown = r.mod->handle_ask(ask_body("stranger", 0, "hi"), "s3cret");
  EXPECT_FALSE(unknown.value("ok", true));
  EXPECT_EQ(unknown.value("error", ""), "forbidden");   // indistinguishable from bad secret
  EXPECT_FALSE(reached);
}

TEST(BridgeModule, HopLimitRejected) {
  Rig r;
  auto res = r.mod->handle_ask(ask_body("front", 1, "hi"), "s3cret");   // max_hops default 1
  EXPECT_FALSE(res.value("ok", true));
  EXPECT_NE(res.value("error", "").find("hop"), std::string::npos);
}

TEST(BridgeModule, MalformedAskRejectedWithoutTurn) {
  Rig r;
  bool reached = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { reached = true; });
  EXPECT_FALSE(r.mod->handle_ask("garbage", "s3cret").value("ok", true));
  EXPECT_FALSE(reached);
}

TEST(BridgeModule, ConfirmGatedActionIsAutoDenied) {
  Rig r(false);
  // Script an agent whose turn raises a confirm; the module must post the denial and the
  // "agent" then finishes the turn (mirrors the Arbiter's confirm continuation).
  nlohmann::json confirm_response;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "run shell?"}}, "arbiter");
  });
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_response = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "[declined]", "arbiter");
  });
  auto res = r.mod->handle_ask(ask_body("front", 0, "do risky thing"), "s3cret");
  ASSERT_TRUE(res.value("ok", false)) << res.dump();
  ASSERT_TRUE(confirm_response.is_object());
  EXPECT_EQ(confirm_response.value("id", ""), "c1");
  EXPECT_FALSE(confirm_response.value("approved", true));
  // The reply carries the standing auto-deny note for the asker's LLM.
  EXPECT_NE(res.value("reply", "").find("auto-denied"), std::string::npos);
}

TEST(BridgeModule, ForeignTurnConfirmIsNotAnswered) {
  Rig r(false);
  bool responded = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { responded = true; });
  // A confirm arriving OUTSIDE a bridge-driven turn (REPL/web turn) is not the bridge's.
  r.bb.post("CONFIRM_REQUEST", {{"id", "x"}, {"prompt", "p"}}, "arbiter");
  r.bb.pump();
  EXPECT_FALSE(responded);
}

TEST(BridgeModule, ShareStoresPrefixedKey) {
  Rig r;
  auto res = r.mod->handle_share(build_share("front", "STATUS", "sunny").dump(), "s3cret");
  EXPECT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "sunny");
}

TEST(BridgeModule, InboundCardShareStoredUnderCardKey) {
  Rig r;
  auto res = r.mod->handle_share(
      build_share("front", "card", {{"name", "front"}}, kShareTypeCard).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.card");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("name", ""), "front");
}

TEST(BridgeModule, InboundFactFromTrustedPeerLabeledReports) {
  Rig r;                                                // "front" not in trust map -> trusted
  auto res = r.mod->handle_share(
      build_share("front", "weather", "sunny", kShareTypeFact).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.fact.weather");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("trust", ""), "trusted");
  EXPECT_EQ(e->value.value("text", ""), "sunny");
  EXPECT_EQ(e->value.value("from", ""), "front");
}

TEST(BridgeModule, InboundFactFromUntrustedPeerLabeledUnverified) {
  Rig r;
  r.mod->set_peer_trust({{"front", false}});
  auto res = r.mod->handle_share(
      build_share("front", "weather", "sunny", kShareTypeFact).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.fact.weather");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("trust", ""), "untrusted");
}

TEST(BridgeModule, InboundRawShareUnchangedLegacy) {
  Rig r;                                                // no type -> raw (Task 2 default)
  auto j = nlohmann::json{{"v", kBridgeProtocolV}, {"from", "front"}, {"key", "STATUS"},
                          {"value", "wet"}};
  auto res = r.mod->handle_share(j.dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "wet");        // legacy PEER.<from>.<key>
}

TEST(BridgeModule, ShareRejectsForbiddenOversizedAndMalformed) {
  Rig r;
  EXPECT_EQ(r.mod->handle_share(build_share("front", "K", 1).dump(), "wrong")
                .value("error", ""), "forbidden");
  EXPECT_EQ(r.mod->handle_share(build_share("ghost", "K", 1).dump(), "s3cret")
                .value("error", ""), "forbidden");
  const std::string big = build_share("front", "K", std::string(kMaxShareBytes, 'x')).dump();
  EXPECT_FALSE(r.mod->handle_share(big, "s3cret").value("ok", true));      // > 64 KB body
  EXPECT_FALSE(r.mod->handle_share("garbage", "s3cret").value("ok", true));
  EXPECT_FALSE(r.bb.get("PEER.front.K").has_value());
}

TEST(BridgeModule, HealthReportsNameAndVersion) {
  Rig r;
  auto h = r.mod->health_json();
  EXPECT_EQ(h.value("name", ""), "worker1");
  EXPECT_EQ(h.value("v", 0), 1);
}

TEST(BridgeModule, AskTimeoutAbandonsTurn) {
  Rig r(false);                                   // NO responder -> run_until must time out
  r.mod->set_turn_timeout_s(0.05);
  bool abandoned = false;
  r.bb.subscribe("TURN_ABANDONED", [&](const Entry&) { abandoned = true; });
  auto res = r.mod->handle_ask(ask_body("front", 0, "hi"), "s3cret");
  EXPECT_FALSE(res.value("ok", true));
  EXPECT_NE(res.value("error", "").find("timed out"), std::string::npos);
  EXPECT_TRUE(abandoned);
}

namespace {
struct FakeHttp : BridgeHttp {
  std::vector<std::tuple<std::string, std::string, std::string>> posts;  // url, body, secret
  int status = 200;
  std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                        const std::string& secret, double) override {
    posts.emplace_back(url, body, secret);
    return {status, R"({"ok":true})"};
  }
  std::vector<std::string> gets;         // discovery: pulled /card urls
  int get_status = 200;
  std::string card_body = R"({"v":1})";
  std::pair<int, std::string> get_json(const std::string& url, const std::string&,
                                       double) override {
    gets.push_back(url);
    return {get_status, card_body};
  }
};
}  // namespace

TEST(BridgeModule, ShareOutPushesToAllPeersOnChange) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg;
  cfg.kv["name"] = "worker1";
  cfg.kv["share_out"] = "STATUS BUDGET_SPENT_USD";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}, {"other", "http://10.0.0.2:9090"}});
  m.on_attach(bb);
  h->posts.clear();                              // drop the on_attach card self-announce noise
  bb.post("STATUS", "sunny", "t");
  bb.pump();                                     // no executor -> push runs inline
  ASSERT_EQ(h->posts.size(), 2u);                // one per peer
  EXPECT_EQ(std::get<0>(h->posts[0]), "http://10.0.0.1:9090/share");
  EXPECT_EQ(std::get<2>(h->posts[0]), "s3cret");
  auto sent = parse_share(std::get<1>(h->posts[0]));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.from, "worker1");
  EXPECT_EQ(sent.key, "STATUS");
  EXPECT_EQ(sent.value.get<std::string>(), "sunny");
}

namespace {
// A deliberately slow seam: sleeps in post_json so a submitted push job is STILL running when
// the module is destroyed. Records into EXTERNAL counters (owned by the test, outlive the http
// object) so the assertion is valid after the job's shared_ptr copy releases the object. The
// mutex guards the worker-thread write (TSan lane runs this too).
struct SlowMutexHttp : BridgeHttp {
  std::mutex* mu;
  int* calls;
  SlowMutexHttp(std::mutex* m, int* c) : mu(m), calls(c) {}
  std::pair<int, std::string> post_json(const std::string&, const std::string&,
                                        const std::string&, double) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // job in-flight at ~BridgeModule
    std::lock_guard<std::mutex> lk(*mu);
    ++*calls;
    return {200, R"({"ok":true})"};
  }
  std::pair<int, std::string> get_json(const std::string&, const std::string&, double) override {
    return {0, ""};   // unused by the push test; present so the pure virtual is satisfied
  }
};
}  // namespace

// Regression (final-review Important): the offloaded share-push job must survive ~BridgeModule.
// ~Executor DRAINS queued jobs, and the Agent destroys the bridge BEFORE the executor — so a job
// that dereferenced the module would use-after-free. The job captures only values, so it runs
// clean on a freed module. Order mirrors the Agent: Blackboard, then Executor, then the module.
TEST(BridgeModule, SharePushJobSurvivesModuleTeardown) {
  std::mutex hmu;
  int calls = 0;
  Blackboard bb;                                   // declared first -> outlives the drain
  {
    Executor ex(2);                                // before the module -> destroyed AFTER it
    {
      BridgeModule m(std::make_unique<SlowMutexHttp>(&hmu, &calls), "s3cret");
      Block cfg; cfg.kv["name"] = "worker1"; cfg.kv["share_out"] = "STATUS";
      m.on_start(cfg, bb);
      m.set_executor(&ex);
      m.on_attach(bb);                             // announce: empty peers -> no card job submitted
      m.set_peers({{"front", "http://10.0.0.1:9090"}});   // set AFTER attach: measure only STATUS
      bb.post("STATUS", "sunny", "t");
      bb.pump();                                   // handler snapshots + submits the slow job
    }                                              // ~BridgeModule while the job sleeps in post_json
  }                                                // ~Executor drains + joins the in-flight job
  std::lock_guard<std::mutex> lk(hmu);
  EXPECT_EQ(calls, 1);                             // push was attempted, no crash on the freed module
}

TEST(BridgeModule, UnlistedKeyIsNotPushed) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1"; cfg.kv["share_out"] = "STATUS";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  h->posts.clear();                              // drop the on_attach card self-announce noise
  bb.post("OTHER_KEY", 1, "t");
  bb.pump();
  EXPECT_TRUE(h->posts.empty());
}

TEST(BridgeModule, FailedPushPostsBridgeErrorAndDoesNotThrow) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->status = 0;                              // transport failure
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1"; cfg.kv["share_out"] = "STATUS";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});   // set AFTER attach: boot announce (empty
                                                       // peers) is a no-op -> no spurious card error
  std::string err;
  bb.subscribe("BRIDGE_ERROR", [&](const Entry& e) {
    if (e.value.is_string()) err = e.value.get<std::string>();
  });
  bb.post("STATUS", "x", "t");
  bb.pump();
  bb.pump();                                     // dispatch the BRIDGE_ERROR posted inline
  EXPECT_NE(err.find("front"), std::string::npos);
}

TEST(BridgeModule, CardJsonAssemblesInjectedAndBusInputs) {
  Rig r(false);
  r.mod->set_description("worker one");
  r.mod->set_tools(nlohmann::json::array({{{"name", "shell"}}}));
  r.mod->set_caps({{"fs_read", "scoped"}, {"net", "public"}});
  r.bb.post("SKILLS_ANNOUNCE",
            "Available skills (…):\n- deploy: ship it", "skills");   // latest-value, get() sees it
  auto c = r.mod->card_json();
  EXPECT_EQ(c.value("name", ""), "worker1");
  EXPECT_EQ(c.value("description", ""), "worker one");
  EXPECT_EQ(c["skills"][0].value("id", ""), "deploy");
  EXPECT_EQ(c["tools"][0].value("name", ""), "shell");
  EXPECT_EQ(c["caps"].value("fs_read", ""), "scoped");
  // no literal path leak from a real capability summary shape
  EXPECT_EQ(c.dump().find("workspace"), std::string::npos);
}

TEST(BridgeModule, CardJsonEmptyWhenNoSkillsOrInjection) {
  Rig r(false);
  auto c = r.mod->card_json();
  EXPECT_EQ(c.value("name", ""), "worker1");
  EXPECT_TRUE(c["skills"].is_array());
  EXPECT_TRUE(c["skills"].empty());              // no SKILLS_ANNOUNCE -> []
}

TEST(BridgeModule, RealSocketAskShareHealthAnd403) {
  Rig r;                                          // echo agent, secret s3cret, peer front
  const int port = r.mod->start_listening();      // port_ default 9090? Rig sets none -> use 0
  ASSERT_GT(port, 0);
  // cpr, not httplib::Client: under TSan glibc 2.42 getaddrinfo spawns a helper thread libtsan
  // can't intercept -> SEGV in the sanitizer allocator. cpr's path is the TSan-safe one (per e2e).
  const std::string base = "http://127.0.0.1:" + std::to_string(port);
  const cpr::Header json_secret{{"Content-Type", "application/json"}, {"X-Hades-Bridge", "s3cret"}};

  // /health with auth
  auto h = cpr::Get(cpr::Url{base + "/health"}, cpr::Header{{"X-Hades-Bridge", "s3cret"}});
  EXPECT_EQ(h.status_code, 200);
  EXPECT_NE(h.text.find("worker1"), std::string::npos);
  // /health without auth -> 403
  auto h403 = cpr::Get(cpr::Url{base + "/health"});
  EXPECT_EQ(h403.status_code, 403);

  // /ask end-to-end over the socket
  auto a = cpr::Post(cpr::Url{base + "/ask"}, cpr::Body{build_ask("front", 0, "ping").dump()},
                     json_secret);
  EXPECT_EQ(a.status_code, 200);
  auto aj = nlohmann::json::parse(a.text, nullptr, false);
  ASSERT_TRUE(aj.value("ok", false));
  EXPECT_NE(aj.value("reply", "").find("ping"), std::string::npos);

  // /ask with a bad secret -> 403
  auto f = cpr::Post(cpr::Url{base + "/ask"}, cpr::Body{build_ask("front", 0, "ping").dump()},
                     cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", "wrong"}});
  EXPECT_EQ(f.status_code, 403);

  // /share over the socket
  auto s = cpr::Post(cpr::Url{base + "/share"},
                     cpr::Body{build_share("front", "STATUS", "wet").dump()}, json_secret);
  EXPECT_EQ(s.status_code, 200);
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "wet");

  // /card with auth -> 200 A2A-shaped; without auth -> 403
  auto cok = cpr::Get(cpr::Url{base + "/card"}, cpr::Header{{"X-Hades-Bridge", "s3cret"}});
  EXPECT_EQ(cok.status_code, 200);
  EXPECT_NE(cok.text.find("worker1"), std::string::npos);
  EXPECT_EQ(cpr::Get(cpr::Url{base + "/card"}).status_code, 403);
}

TEST(BridgeModule, DiscoverOncePullsPeerCardsToBus) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->card_body = R"({"name":"front","skills":[{"id":"deploy","description":"ship"}]})";
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  m.discover_once();
  ASSERT_EQ(h->gets.size(), 1u);
  EXPECT_EQ(h->gets[0], "http://10.0.0.1:9090/card");
  auto e = bb.get("PEER.front.card");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("name", ""), "front");
}

TEST(BridgeModule, DiscoverFailureLogsBridgeErrorNoThrow) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->get_status = 0;                                  // transport failure
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  std::string err;
  bb.subscribe("BRIDGE_ERROR", [&](const Entry& e) {
    if (e.value.is_string()) err = e.value.get<std::string>();
  });
  m.discover_once();
  bb.pump();
  EXPECT_FALSE(bb.get("PEER.front.card").has_value());
  EXPECT_NE(err.find("front"), std::string::npos);
}

TEST(BridgeModule, SelfAnnouncesCardToPeersOnAttach) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_description("worker one");
  m.set_peers({{"front", "http://10.0.0.1:9090"}, {"other", "http://10.0.0.2:9090"}});
  m.on_attach(bb);                                     // initial announce (inline: no executor)
  bb.pump();
  ASSERT_EQ(h->posts.size(), 2u);                      // one /share per peer
  EXPECT_EQ(std::get<0>(h->posts[0]), "http://10.0.0.1:9090/share");
  auto sent = parse_share(std::get<1>(h->posts[0]));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.type, "card");
  EXPECT_EQ(sent.key, "card");
  EXPECT_EQ(sent.value.value("name", ""), "worker1");
}

TEST(BridgeModule, ReAnnouncesCardWhenSkillsChange) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  bb.pump();
  const std::size_t after_attach = h->posts.size();    // >=1 initial
  bb.post("SKILLS_ANNOUNCE", "Available skills (…):\n- deploy: ship it", "skills");
  bb.pump();
  ASSERT_GT(h->posts.size(), after_attach);            // re-announced
  auto sent = parse_share(std::get<1>(h->posts.back()));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.value["skills"][0].value("id", ""), "deploy");
}
