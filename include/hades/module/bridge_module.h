// include/hades/module/bridge_module.h — agent↔agent bridge app (pShare analogue)
//
// The inbound half of the multi-agent bridge (spec 2026-07-03): peers reach this agent over
// HTTP with a shared-secret header. POST /ask drives ONE whole turn through the shared
// TurnGate exactly like the REPL/web/Telegram front-ends — the peer's request passes THIS
// agent's own Arbiter/objectives; a confirm-band action is AUTO-DENIED (a peer cannot grant
// human confirmation). POST /share stores the payload under PEER.<from>.<key> (fixed rename
// on arrival — a peer can never inject a local bus key). Security: the secret comes from an
// env var (secret_env, default HADES_BRIDGE_SECRET; NEVER the manifest) and `from` must be a
// rostered Peer — either failure returns an indistinguishable "forbidden". The socket layer
// (Task 4) is a thin shell over the socket-free handle_ask/handle_share seams below.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/bridge/http.h"
#include "hades/module.h"
#include "hades/turn_gate.h"

// Forward-declared so this public header does NOT pull in the heavy <httplib.h> for every
// TU that includes it; the .cpp (and the socket test) include the real header. The dtor
// must therefore live in the .cpp where httplib::Server is a complete type.
namespace httplib {
class Server;
}

namespace hades {
class Blackboard;
class Executor;

class BridgeModule : public Module {
 public:
  // Test injection: a non-empty secret skips the on_start env-var resolution. Defined
  // out-of-line in the .cpp (like the dtor) because a member is unique_ptr<httplib::Server>
  // (incomplete here) — an inline body would force <httplib.h> onto every TU that constructs a
  // BridgeModule (e.g. the agent-wiring factory).
  explicit BridgeModule(std::string secret_for_test = "");
  // Test injection of the outbound HTTP seam (real path: on_start creates CprBridgeHttp).
  explicit BridgeModule(std::unique_ptr<BridgeHttp> http, std::string secret_for_test = "");
  void set_executor(Executor* ex) { executor_ = ex; }
  ~BridgeModule() override;   // stop + join the listener thread
  std::string type() const override { return "bridge"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  // Bind (port 0 -> any free port) and serve on a background thread. Called by hades_main
  // AFTER the graph is wired (never by on_attach — tests spawn no thread unless they ask).
  // Returns the bound port, or -1 on bind failure. Idempotent.
  int start_listening();
  int port() const { return port_; }
  void wait();                // join (bridge-only roster blocks here; Ctrl-C exits)

  void set_peers(std::map<std::string, std::string> peers) { peers_ = std::move(peers); }
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  // Socket-free request handlers (the Task-4 routes are thin shells over these).
  // presented_secret is the X-Hades-Bridge header value; a bad secret OR an unknown `from`
  // returns {"ok":false,"error":"forbidden"} (indistinguishable — no info leak).
  nlohmann::json handle_ask(const std::string& body, const std::string& presented_secret);
  nlohmann::json handle_share(const std::string& body, const std::string& presented_secret);
  nlohmann::json health_json() const;

  const std::string& name() const { return name_; }
  double ask_timeout_s() const { return ask_timeout_s_; }
  const std::vector<std::string>& share_out_keys() const { return share_out_; }

 private:
  std::string host_ = "127.0.0.1";
  int port_ = 9090;
  bool authorized_(const std::string& presented_secret, const std::string& from) const;
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::string name_;                       // REQUIRED (valid_peer_name)
  std::string secret_;                     // resolved once (env or test injection)
  std::map<std::string, std::string> peers_;   // name -> base url (allowlist + push targets)
  std::vector<std::string> share_out_;     // keys pushed to peers on change (Task 3)
  long long max_hops_ = 1;
  double ask_timeout_s_ = 180.0;           // kDefaultAskTimeoutS (set in on_start)
  double turn_timeout_override_s_ = 0.0;

  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;

  // Turn-capture state (request thread only, under the gate while a turn runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  std::string last_reply_;
  bool denied_confirm_ = false;            // a confirm was auto-denied during MY turn

  // shared_ptr (not unique): a share-push job copies this handle so the seam stays ALIVE if the
  // job drains AFTER ~BridgeModule. The injection ctor takes a unique_ptr and converts on
  // assignment. Real path: on_start makes a CprBridgeHttp.
  std::shared_ptr<BridgeHttp> http_;
  // Push jobs run here when set (inline otherwise — tests). TEARDOWN GUARANTEE: a submitted push
  // job captures ONLY values — a raw Blackboard* (outlives the executor drain) plus copies of
  // name/secret/peers and the http shared_ptr — never `this`. So a job draining during Executor
  // teardown (the Agent destroys `bridge` BEFORE `executor`, and ~Executor drains queued jobs)
  // touches nothing owned by the destroyed module. See run_share_push in the .cpp.
  Executor* executor_ = nullptr;

  std::unique_ptr<httplib::Server> srv_;   // dtor lives in the .cpp (complete type there)
  std::thread listen_thread_;
};
}  // namespace hades
