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
#include <nlohmann/json.hpp>
#include "hades/module.h"
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

private:
  nlohmann::json collect_();  // after a post, pump the turn and read the captured result

  Blackboard* bb_ = nullptr;
  std::mutex mu_;
  std::string last_reply_;
  bool got_reply_ = false;
  nlohmann::json pending_confirm_;  // null when no confirm is outstanding
};
}  // namespace hades
