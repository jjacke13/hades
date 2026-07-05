// src/apps/bridge/bridge.cpp — the agent↔agent bridge app: module + wire protocol + cpr shell
//
// Merged (2026-07-04 src reorg): module/bridge_module (inbound /ask peer turns w/ confirm
// auto-deny, /share PEER.<from>.<key> ingest, listener thread, executor share push) +
// bridge/protocol (versioned tolerant parse/build, peer-name gate) + bridge/cpr_bridge_http
// (thin outbound shell). valid_peer_name stays header-inline in include/hades/bridge/protocol.h.

#include <cpr/cpr.h>
#include <cstdlib>
#include <exception>
#include <httplib.h>
#include <sstream>
#include "hades/module/bridge_module.h"
#include "hades/blackboard.h"
#include "hades/bridge/http.h"
#include "hades/bridge/protocol.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/launcher.h"    // MalConfig
#include "hades/timeouts.h"    // kDefaultTurnIdleTimeoutS, kDefaultAskTimeoutS

// ── BridgeModule: auth, allowlist, peer turns, share ingest, listener thread (was src/module/bridge_module.cpp) ──────────────
namespace hades {
namespace {
// The outbound share push, as a SELF-CONTAINED job: it owns everything it touches by value/copy
// and captures NO BridgeModule. This is the teardown fix (final-review Important) — the push is
// offloaded to the Executor, and ~Executor DRAINS queued jobs, but the Agent destroys the bridge
// BEFORE the executor. A job that dereferenced a Bridge*  (name_/peers_/http_/bb_) would then run
// on a freed module. Precedent: the LLM offload worker in llm_module.cpp captures only what it
// needs, never `this`.
//   `bb` is a raw Blackboard*: SAFE because hades_main declares the Blackboard BEFORE the Agent
//   (and every test declares bb before the module), so the bus outlives the executor drain.
//   `http` is a shared_ptr copy that keeps the seam alive independent of the module's lifetime.
// Best-effort: a peer being down must never disturb the agent (status 0 = transport failure; any
// non-2xx counts as failed). This runs on the pump thread on the inline path (no executor), which
// must NEVER throw — so the WHOLE body (incl. build_share().dump()) is under an outer try/catch;
// the per-peer catch stays so one failing peer never stops the rest. BRIDGE_ERROR is observable.
void run_share_push(Blackboard* bb, std::shared_ptr<BridgeHttp> http, const std::string& name,
                    const std::string& secret,
                    const std::map<std::string, std::string>& peers, const std::string& key,
                    const nlohmann::json& value) {
  try {
    const std::string body = build_share(name, key, value).dump();
    for (const auto& [peer, url] : peers) {
      try {
        auto [status, resp] = http->post_json(url + "/share", body, secret, 10.0);
        if (status < 200 || status >= 300)
          bb->post("BRIDGE_ERROR", "share push to " + peer + " failed (status " +
                                       std::to_string(status) + ")", "bridge");
      } catch (...) {
        bb->post("BRIDGE_ERROR", "share push to " + peer + " failed (exception)", "bridge");
      }
    }
  } catch (...) {
    bb->post("BRIDGE_ERROR", "share push failed (exception building payload)", "bridge");
  }
}
}  // namespace

// Out-of-line (the header only forward-declares httplib::Server; a member unique_ptr to it
// needs the complete type at the point of construction — this TU includes <httplib.h>).
BridgeModule::BridgeModule(std::string secret_for_test) : secret_(std::move(secret_for_test)) {}
BridgeModule::BridgeModule(std::unique_ptr<BridgeHttp> http, std::string secret_for_test)
    : BridgeModule(std::move(secret_for_test)) {   // delegate: no member-order coupling
  http_ = std::move(http);
}

double BridgeModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

BridgeModule::~BridgeModule() {
  if (srv_) srv_->stop();                          // wakes the listen() thread
  if (listen_thread_.joinable()) listen_thread_.join();
}

int BridgeModule::start_listening() {
  if (listen_thread_.joinable()) return port_;     // idempotent
  srv_ = std::make_unique<httplib::Server>();
  // Socket timeouts above the turn idle ceiling so a long-but-legit /ask is never cut off
  // mid-turn (HttpServerModule::configure_server_ rationale).
  const time_t secs = static_cast<time_t>(effective_timeout_()) + 60;
  srv_->set_read_timeout(secs, 0);
  srv_->set_write_timeout(secs, 0);

  // Routes are thin shells over the socket-free handlers; a "forbidden" result maps to 403.
  auto respond = [](httplib::Response& res, const nlohmann::json& out) {
    if (out.value("error", "") == "forbidden") res.status = 403;
    res.set_content(out.dump(), "application/json");
  };
  srv_->Post("/ask", [this, respond](const httplib::Request& req, httplib::Response& res) {
    respond(res, handle_ask(req.body, req.get_header_value("X-Hades-Bridge")));
  });
  srv_->Post("/share", [this, respond](const httplib::Request& req, httplib::Response& res) {
    respond(res, handle_share(req.body, req.get_header_value("X-Hades-Bridge")));
  });
  srv_->Get("/health", [this, respond](const httplib::Request& req, httplib::Response& res) {
    // Auth on /health too (spec): liveness is fleet-internal, not public.
    if (req.get_header_value("X-Hades-Bridge") != secret_) {
      respond(res, {{"ok", false}, {"error", "forbidden"}});
      return;
    }
    respond(res, health_json());
  });

  // Bind BEFORE spawning the thread so the caller learns the real port synchronously
  // (port 0 -> ephemeral; tests use this so parallel runs never collide).
  if (port_ == 0) {
    const int bound = srv_->bind_to_any_port(host_);
    if (bound <= 0) { srv_.reset(); return -1; }
    port_ = bound;
  } else if (!srv_->bind_to_port(host_, port_)) {
    srv_.reset();
    return -1;
  }
  listen_thread_ = std::thread([this] { srv_->listen_after_bind(); });
  return port_;
}

void BridgeModule::wait() {
  if (listen_thread_.joinable()) listen_thread_.join();
}

void BridgeModule::on_start(const Block& cfg, Blackboard&) {
  // The agent's bridge identity. REQUIRED: it is the `from` peers verify us by, half of the
  // TURN_ORIGIN value, and the PEER.<name>. prefix on the receiving side. Fail fast + loud.
  if (!cfg.kv.count("name") || !valid_peer_name(cfg.kv.at("name")))
    throw MalConfig("bridge module requires a valid name ([A-Za-z0-9_-]{1,64})");
  name_ = cfg.kv.at("name");
  if (cfg.kv.count("host")) host_ = cfg.kv.at("host");
  if (cfg.kv.count("port")) {
    // Explicit branch, not set_pos_double_on_string: port 0 (bind-any-ephemeral) is a legal
    // value the positive-only helper would reject.
    try { port_ = std::stoi(cfg.kv.at("port")); } catch (const std::exception&) {}
    if (port_ < 0 || port_ > 65535) port_ = 9090;
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
  if (!http_) http_ = std::make_shared<CprBridgeHttp>();
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
      // This handler runs on the PUMP thread while the module is alive (like every subscription),
      // so reading the members here is safe. It snapshots them into a job that captures NO `this`
      // (see run_share_push): the job may drain on an Executor worker AFTER ~BridgeModule, so it
      // must own everything it touches. bb_ is a raw ptr (the Blackboard outlives the drain);
      // peers_ is a small per-push map copy; http_ shared_ptr copy keeps the seam alive.
      Blackboard* bb = bb_;
      std::shared_ptr<BridgeHttp> http = http_;
      std::string name = name_, secret = secret_;
      std::map<std::string, std::string> peers = peers_;
      const nlohmann::json value = e.value;
      auto job = [bb, http, name, secret, peers, key, value] {
        run_share_push(bb, http, name, secret, peers, key, value);
      };
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

}  // namespace hades

// ── bridge wire protocol: tolerant parse/build, peer-name gate (was src/bridge/protocol.cpp) ──────────────
namespace hades {
namespace {

// Common envelope checks: valid JSON object, v == kBridgeProtocolV, from is a valid peer
// name. Returns true and fills m.from on success; sets m.error otherwise.
bool parse_envelope(const nlohmann::json& j, BridgeMsg& m) {
  if (!j.is_object()) { m.error = "not a JSON object"; return false; }
  auto v = j.find("v");
  if (v == j.end() || !v->is_number_integer() ||
      v->get<long long>() != kBridgeProtocolV) {
    m.error = "unsupported protocol version";
    return false;
  }
  auto from = j.find("from");
  if (from == j.end() || !from->is_string() || !valid_peer_name(from->get<std::string>())) {
    m.error = "missing/invalid from";
    return false;
  }
  m.from = from->get<std::string>();
  return true;
}

bool valid_share_key(const std::string& k) {
  if (k.empty() || k.size() > 128) return false;
  for (char c : k)
    if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) >= 127)
      return false;   // no whitespace/control/non-ASCII in a bus key
  return true;
}

}  // namespace

nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"hops", hops}, {"message", message}};
}

nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value, const std::string& type) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"key", key}, {"value", value}, {"type", type}};
}

BridgeMsg parse_ask(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto hops = j.find("hops");
  if (hops == j.end() || !hops->is_number_integer() || hops->get<long long>() < 0) {
    m.error = "missing/invalid hops";
    return m;
  }
  m.hops = hops->get<long long>();
  auto msg = j.find("message");
  if (msg == j.end() || !msg->is_string() || msg->get<std::string>().empty()) {
    m.error = "missing/invalid message";
    return m;
  }
  m.message = msg->get<std::string>();
  m.ok = true;
  return m;
}

BridgeMsg parse_share(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto key = j.find("key");
  if (key == j.end() || !key->is_string() || !valid_share_key(key->get<std::string>())) {
    m.error = "missing/invalid key";
    return m;
  }
  m.key = key->get<std::string>();
  auto val = j.find("value");
  if (val == j.end()) { m.error = "missing value"; return m; }
  m.value = *val;
  auto ty = j.find("type");                       // tolerant: absent / non-string -> "raw"
  m.type = (ty != j.end() && ty->is_string()) ? ty->get<std::string>() : "raw";
  m.ok = true;
  return m;
}

std::string peer_bus_key(const std::string& from, const std::string& key) {
  return "PEER." + from + "." + key;
}
}  // namespace hades

// ── CprBridgeHttp: thin cpr shell for outbound bridge requests (was src/bridge/cpr_bridge_http.cpp) ──────────────
namespace hades {
std::pair<int, std::string> CprBridgeHttp::post_json(const std::string& url,
                                                     const std::string& body,
                                                     const std::string& secret,
                                                     double timeout_s) {
  try {
    cpr::Response r = cpr::Post(
        cpr::Url{url}, cpr::Body{body},
        cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", secret}},
        cpr::Timeout{static_cast<int>(timeout_s * 1000)}, cpr::Redirect{false});
    return {static_cast<int>(r.status_code), r.text};
  } catch (const std::exception&) {
    return {0, ""};   // transport failure — the caller degrades (never throws)
  }
}
}  // namespace hades

// ── bridge registry: canonical card builders (registry.h) ──────────────
#include "hades/bridge/registry.h"
#include <sstream>
namespace hades {

nlohmann::json build_skills_from_announce(const std::string& announce) {
  nlohmann::json out = nlohmann::json::array();
  std::istringstream is(announce);
  std::string line;
  while (std::getline(is, line)) {
    if (line.rfind("- ", 0) != 0) continue;              // only "- id: desc" list lines
    const std::size_t colon = line.find(": ", 2);
    if (colon == std::string::npos) continue;
    std::string id = line.substr(2, colon - 2);
    std::string desc = line.substr(colon + 2);
    if (id.empty()) continue;
    out.push_back({{"id", id}, {"description", desc}});
  }
  return out;
}

nlohmann::json caps_summary(const Block& cfg) {
  auto has = [&](const char* k) { return cfg.kv.count(k) && !cfg.kv.at(k).empty(); };
  bool block_priv = false;
  if (cfg.kv.count("block_private_net")) {
    const std::string& v = cfg.kv.at("block_private_net");
    block_priv = (v == "true" || v == "1" || v == "yes");
  }
  return {{"fs_read",  has("fs_read_allow")  ? "scoped" : "none"},
          {"fs_write", has("fs_write_allow") ? "scoped" : "none"},
          {"exec",     has("exec_allow")     ? "scoped" : "none"},
          {"net",      block_priv ? "private-blocked" : "public"}};
}

nlohmann::json build_card(const std::string& name, const std::string& url, int version,
                          const std::string& description, const std::string& skills_announce,
                          const nlohmann::json& tools, const nlohmann::json& caps) {
  return {{"name", name},
          {"description", description.empty() ? name : description},
          {"url", url},
          {"version", version},
          {"capabilities", {{"streaming", false}}},
          {"skills", build_skills_from_announce(skills_announce)},
          {"tools", tools.is_array() ? tools : nlohmann::json::array()},
          {"caps", caps.is_object() ? caps : nlohmann::json::object()}};
}

}  // namespace hades
