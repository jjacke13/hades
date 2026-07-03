// src/module/bridge_module.cpp — inbound bridge: auth, allowlist, peer turns, share ingest
#include "hades/module/bridge_module.h"
#include <cstdlib>
#include <sstream>
#include "hades/blackboard.h"
#include "hades/bridge/protocol.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/launcher.h"    // MalConfig
#include "hades/timeouts.h"    // kDefaultTurnIdleTimeoutS, kDefaultAskTimeoutS
namespace hades {

double BridgeModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void BridgeModule::on_start(const Block& cfg, Blackboard&) {
  // The agent's bridge identity. REQUIRED: it is the `from` peers verify us by, half of the
  // TURN_ORIGIN value, and the PEER.<name>. prefix on the receiving side. Fail fast + loud.
  if (!cfg.kv.count("name") || !valid_peer_name(cfg.kv.at("name")))
    throw MalConfig("bridge module requires a valid name ([A-Za-z0-9_-]{1,64})");
  name_ = cfg.kv.at("name");
  if (cfg.kv.count("host")) host_ = cfg.kv.at("host");
  if (cfg.kv.count("port")) {
    double p = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("port"), p)) port_ = static_cast<int>(p);
  }
  if (cfg.kv.count("max_hops")) {
    double h = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("max_hops"), h)) max_hops_ = static_cast<long long>(h);
  }
  ask_timeout_s_ = kDefaultAskTimeoutS;
  if (cfg.kv.count("ask_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("ask_timeout_s"), t)) ask_timeout_s_ = t;
  }
  if (cfg.kv.count("share_out")) {
    std::istringstream is(cfg.kv.at("share_out"));
    std::string k;
    while (is >> k) share_out_.push_back(k);
  }
  if (secret_.empty()) {                    // not injected (tests) -> resolve the env var
    const std::string env =
        cfg.kv.count("secret_env") ? cfg.kv.at("secret_env") : "HADES_BRIDGE_SECRET";
    const char* sec = std::getenv(env.c_str());
    if (!sec || !*sec)
      throw MalConfig("bridge secret env var not set or empty: " + env);
    secret_ = sec;
  }
  if (!http_) http_ = std::make_unique<CprBridgeHttp>();
}

void BridgeModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Turn-owner guard: capture ONLY for turns this module drives (my_turn_) — a REPL/web/
  // Telegram turn's reply or confirm is not ours (symmetric to the other front-ends).
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    // AUTO-DENY (spec decision): a peer request cannot grant human confirmation. Post the
    // denial immediately — the Arbiter continues the turn to its final reply, and handle_ask
    // appends the standing note so the asker's LLM knows why.
    denied_confirm_ = true;
    auto id = e.value.find("id");
    bb_->post("CONFIRM_RESPONSE",
              {{"id", (id != e.value.end() && id->is_string()) ? id->get<std::string>() : ""},
               {"approved", false}},
              "bridge");
  });
  // Outbound pShare: on a change of any listed key, push it to ALL peers. The handler runs on
  // the pump thread, so the (possibly slow) HTTP posts are offloaded to the Executor when one
  // is set; without one (tests) they run inline and stay deterministic. Fire-and-forget.
  for (const auto& key : share_out_) {
    bb.subscribe(key, [this, key](const Entry& e) {
      // Capture by value: the worker must not touch `e` after the handler returns.
      const nlohmann::json value = e.value;
      auto job = [this, key, value] { push_share_(key, value); };
      if (executor_) executor_->submit(job);
      else job();
    });
  }
}

bool BridgeModule::authorized_(const std::string& presented_secret,
                               const std::string& from) const {
  // One combined answer: a bad secret and an unknown peer are indistinguishable (no info leak).
  return !secret_.empty() && presented_secret == secret_ && peers_.count(from) > 0;
}

nlohmann::json BridgeModule::handle_ask(const std::string& body,
                                        const std::string& presented_secret) {
  BridgeMsg m = parse_ask(body);
  if (!m.ok) return {{"ok", false}, {"error", m.error}};
  if (!authorized_(presented_secret, m.from)) return {{"ok", false}, {"error", "forbidden"}};
  if (m.hops >= max_hops_) return {{"ok", false}, {"error", "hop limit exceeded"}};

  std::lock_guard<std::mutex> lk(turn_mu_());     // one turn at a time across ALL front-ends
  my_turn_ = true;
  // RAII reset declared AFTER the lock: runs BEFORE the mutex releases on EVERY exit path.
  struct Reset { bool& f; ~Reset() { f = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  denied_confirm_ = false;
  // Origin BEFORE the message: the latest-value map updates on post, so the PeerLoopGuard
  // (and anything else) sees "peer:<name>" for the whole turn.
  bb_->post("TURN_ORIGIN", "peer:" + m.from, "bridge");
  bb_->post("USER_MESSAGE", "(from peer agent \"" + m.from + "\") " + m.message, "bridge");
  const bool done = bb_->run_until([this] { return got_reply_; }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "bridge");
    bb_->pump();
    return {{"ok", false}, {"error", "turn timed out"}};
  }
  std::string reply = last_reply_;
  if (denied_confirm_)
    reply += "\n[note: a confirm-gated action was auto-denied — peer requests cannot grant "
             "human confirmation]";
  return {{"ok", true}, {"reply", reply}};
}

nlohmann::json BridgeModule::handle_share(const std::string& body,
                                          const std::string& presented_secret) {
  if (body.size() > kMaxShareBytes) return {{"ok", false}, {"error", "payload too large"}};
  BridgeMsg m = parse_share(body);
  if (!m.ok) return {{"ok", false}, {"error", m.error}};
  if (!authorized_(presented_secret, m.from)) return {{"ok", false}, {"error", "forbidden"}};
  // No turn, no gate: thread-safe post; the PEER. prefix is collision-proof by construction.
  bb_->post(peer_bus_key(m.from, m.key), m.value, "bridge");
  return {{"ok", true}};
}

nlohmann::json BridgeModule::health_json() const {
  return {{"name", name_}, {"v", kBridgeProtocolV}};
}

void BridgeModule::push_share_(const std::string& key, const nlohmann::json& value) {
  // Best-effort: a peer being down must never disturb the agent. status 0 = transport failure;
  // any non-2xx counts as failed too. BRIDGE_ERROR is observable in hades-scope and tests.
  const std::string body = build_share(name_, key, value).dump();
  for (const auto& [peer, url] : peers_) {
    try {
      auto [status, resp] = http_->post_json(url + "/share", body, secret_, 10.0);
      if (status < 200 || status >= 300)
        bb_->post("BRIDGE_ERROR", "share push to " + peer + " failed (status " +
                                      std::to_string(status) + ")", "bridge");
    } catch (...) {
      bb_->post("BRIDGE_ERROR", "share push to " + peer + " failed (exception)", "bridge");
    }
  }
}
}  // namespace hades
