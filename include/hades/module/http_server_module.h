// include/hades/module/http_server_module.h — HTTP front-end module (the `--serve` mode)
//
// A networked alternative to the stdin ChatModule: bridges HTTP requests to the
// Blackboard. POST /chat drives one turn (post USER_MESSAGE -> pump -> capture the
// ASSISTANT_MESSAGE); a confirm-gated action returns {needs_confirm,id,prompt} and is
// resolved by POST /confirm (the Arbiter's pending slot survives between the two
// requests). All Blackboard access is serialized under one mutex (the bus is
// single-threaded). handle_message/handle_confirm are socket-free for testing.

#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "hades/module.h"

// Forward-declared so the public header does NOT pull in the heavy <httplib.h> for
// every translation unit; the .cpp (and the serve test) include the real header.
namespace httplib {
class Server;
struct Request;
}

namespace hades {
class Blackboard;

class HttpServerModule : public Module {
public:
  std::string type() const override { return "serve"; }
  void on_attach(Blackboard& bb) override;

  // Drive one turn from a user message. Returns {"reply": "..."} on a completed
  // answer, or {"needs_confirm": true, "id", "prompt"} when a turn is gated.
  nlohmann::json handle_message(const std::string& text);
  // Resolve a pending confirm; returns the resulting reply (or another needs_confirm).
  nlohmann::json handle_confirm(const std::string& id, bool approved);

  // Blocking: serve the static web UI (webroot mounted at /) + the JSON API
  // (POST /chat, POST /confirm, GET /health) until the process exits. host defaults
  // to loopback at the call site so the agent is not network-exposed by default.
  void listen(const std::string& host, int port, const std::string& webroot);

  // Configured idle ceiling (0 ⇒ default kDefaultTurnIdleTimeoutS = 900s). Wiring sets
  // the manifest's turn_idle_timeout_s through this seam; tests pass a small value to
  // force a fast turn abandonment instead of waiting out the production default.
  void set_collect_timeout_s(double s) { collect_timeout_override_s_ = s; }
  // The effective run_until idle ceiling (override if set, else the default). Exposed so
  // wiring/tests can observe the configured value without binding a socket / driving a turn.
  double idle_timeout_s() const { return effective_collect_timeout_s(); }

  // The --serve front-end reads the same per-session conversation jsonl that the Arbiter persists,
  // so GET /history can re-render a resumed transcript. Wiring sets this to the resolved session
  // path (empty -> history_json() returns {"history":[]}). Read-only; no coupling to the Arbiter.
  void set_session_path(std::string p) { session_path_ = std::move(p); }
  // Socket-free body of GET /history: {"history": [ ...raw stored messages... ]} read from disk.
  // Const + socket-free so a test can assert it without binding a socket (mirrors handle_message).
  nlohmann::json history_json() const;

  // CSRF chokepoint (was a file-local helper; promoted to a public static so a test can assert it
  // without a socket, like configure_server_). Returns false for a request that must be blocked:
  // a tool-invoking POST (/chat,/confirm) or GET /history lacking the X-Hades header that a
  // cross-origin "simple" request cannot add without a preflight we never grant. Static GETs pass.
  static bool authorize(const httplib::Request& req);

  // Apply the socket read/write timeouts to a freshly-constructed httplib::Server before
  // listen(). Raised ABOVE idle_s so a long (up to the idle ceiling) collect_ handler's
  // connection is never dropped. Static + httplib-by-reference so a test can probe it
  // without binding a socket; defined in the .cpp (which owns <httplib.h>).
  static void configure_server_(httplib::Server& srv, double idle_s);

private:
  nlohmann::json collect_();  // after a post, pump the turn and read the captured result
  // run_until idle timeout in seconds: the test/manifest override if set (>0), else the
  // production kCollectTimeoutS default. Defined in http_server_module.cpp so it can see
  // the file-local constant; the single source for both collect_ and the socket timeouts.
  double effective_collect_timeout_s() const;

  Blackboard* bb_ = nullptr;
  std::mutex mu_;
  std::string last_reply_;
  bool got_reply_ = false;
  nlohmann::json pending_confirm_;  // null when no confirm is outstanding
  std::string session_path_;  // per-session jsonl read by GET /history (set by wiring; may be empty)
  // collect_ idle-timeout override (seconds). 0 = use the production default
  // (kCollectTimeoutS in http_server_module.cpp); set_collect_timeout_s gives tests a small value.
  double collect_timeout_override_s_ = 0.0;
};
}  // namespace hades
