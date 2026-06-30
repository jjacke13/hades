// src/module/http_server_module.cpp — HTTP front-end implementation (cpp-httplib)
//
// on_attach subscribes to ASSISTANT_MESSAGE (captures the reply) and CONFIRM_REQUEST
// (captures a pending gate). handle_message/handle_confirm post the triggering event
// and pump the turn to completion under mu_, then report reply-or-needs_confirm.
// listen() exposes POST /chat, POST /confirm, GET /health. See the header.

#include "hades/module/http_server_module.h"
#include <httplib.h>
#include <iostream>
#include "hades/blackboard.h"
#include "hades/session_history.h"  // read_session_jsonl (GET /history)
#include "hades/timeouts.h"   // kDefaultTurnIdleTimeoutS

namespace {
// Generous IDLE ceiling for run_until — NOT a per-turn wall-clock cap. The timer
// resets on every bus event, so it fires only after this many seconds of NO bus
// activity (a genuinely hung/stalled worker), returning "[timed out]" instead of
// blocking the HTTP thread forever. A turn may offload a (possibly slow) LLM call
// onto a worker, so collect_ waits on the bus rather than busy-pumping; a
// legitimately long but bus-active multi-step turn can exceed this wall-clock and is
// NOT killed — only true silence trips it. Socket-free tests run inline (no
// executor) -> the predicate holds during the first pump -> run_until returns at once.
//
// LOAD-BEARING INVARIANT: this idle timeout MUST stay greater than the maximum single
// in-flight poster duration (cpr LLM cap = llm_timeout_s, default kDefaultLlmTimeoutS=600s
// in include/hades/timeouts.h + tool cap ~30s in include/hades/module/tool_runner.h). That
// guarantee is what ensures no worker is still running when run_until abandons a turn, so no
// stale LLM_RESPONSE is produced after abandonment — the turn-epoch (Arbiter::on_llm_response
// freshness gate) is only defense-in-depth. The invariant is now enforced at the build
// boundary: app/agent_wiring.cpp throws MalConfig if turn_idle_timeout_s <= llm_timeout_s.
// If you add tool-offload / SSE that can keep a worker alive past this idle window, you MUST
// harden the epoch (bump it on turn abandonment / drop responses for abandoned turns) — see
// the run_until follow-up spec and the
// DISABLED_StaleResponseDispatchedBeforeNextUserMessageIsAccepted regression test.
constexpr double kCollectTimeoutS = hades::kDefaultTurnIdleTimeoutS;
}  // namespace

namespace hades {

void HttpServerModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (e.value.is_string()) {
      last_reply_ = e.value.get<std::string>();
      got_reply_ = true;
    }
  });
  bb.subscribe("CONFIRM_REQUEST",
               [this](const Entry& e) { pending_confirm_ = e.value; });
}

double HttpServerModule::effective_collect_timeout_s() const {
  return collect_timeout_override_s_ > 0.0 ? collect_timeout_override_s_ : kCollectTimeoutS;
}

void HttpServerModule::configure_server_(httplib::Server& srv, double idle_s) {
  // Socket read/write timeouts bracket socket I/O, NOT handler duration — a long collect_
  // handler (it can run up to the idle ceiling while run_until waits on the bus) is NOT
  // killed by these. We still raise them above the idle ceiling (idle + 60s) so a slow but
  // legitimate turn's connection is never cut off mid-flight (defensive + documents intent).
  // Keep-alive left at the httplib default.
  const time_t secs = static_cast<time_t>(idle_s) + 60;
  srv.set_read_timeout(secs, 0);
  srv.set_write_timeout(secs, 0);
}

bool HttpServerModule::authorize(const httplib::Request& req) {
  // Tool-invoking POSTs and the conversation-returning GET /history require the custom header a
  // cross-origin simple request cannot add without a preflight we never grant. Static GETs (the
  // UI itself) stay exempt so the page can load.
  if (req.method == "POST" && (req.path == "/chat" || req.path == "/confirm"))
    return req.has_header("X-Hades");
  if (req.path == "/history")
    return req.has_header("X-Hades");
  return true;
}

nlohmann::json HttpServerModule::collect_() {
  // Drive the turn to a decision: either the final reply was captured or the turn is
  // gated on a confirm. run_until pumps inline first (synchronous/test path completes
  // immediately) and otherwise sleeps on the bus until a worker posts the result.
  const double timeout_s = effective_collect_timeout_s();
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, timeout_s);
  if (!done) {
    // Idle timeout: the turn is abandoned. Signal it (the Arbiter bumps its turn epoch on
    // TURN_ABANDONED, dropping any late worker response for this turn) and pump so that
    // happens promptly; still under mu_, so the bus stays serialized. Then report the timeout.
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "serve");
    bb_->pump();
    return {{"reply", "[timed out]"}};  // hung worker -> don't block forever
  }
  if (got_reply_) return {{"reply", last_reply_}};
  if (!pending_confirm_.is_null())
    return {{"needs_confirm", true},
            {"id", pending_confirm_.value("id", "")},
            {"prompt", pending_confirm_.value("prompt", "")}};
  return {{"reply", ""}};  // nothing to say
}

nlohmann::json HttpServerModule::history_json() const {
  // Disk read only; no shared mutable state -> no mu_. A concurrent Arbiter append that leaves a
  // half-written final line is skipped by read_session_jsonl's tolerant parse.
  return {{"history", read_session_jsonl(session_path_)}};
}

nlohmann::json HttpServerModule::handle_message(const std::string& text) {
  std::lock_guard<std::mutex> lk(mu_);
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post("USER_MESSAGE", text, "http");
  return collect_();
}

nlohmann::json HttpServerModule::handle_confirm(const std::string& id, bool approved) {
  std::lock_guard<std::mutex> lk(mu_);
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post("CONFIRM_RESPONSE", {{"id", id}, {"approved", approved}}, "http");
  return collect_();
}

void HttpServerModule::listen(const std::string& host, int port, const std::string& webroot) {
  httplib::Server srv;

  // Raise the socket read/write timeouts above the configured idle ceiling so a long
  // collect_ handler's connection is never dropped (see configure_server_).
  configure_server_(srv, effective_collect_timeout_s());

  // Auth chokepoint: runs before routing; a future token check goes in authorize().
  srv.set_pre_routing_handler(
      [](const httplib::Request& req, httplib::Response& res) {
        if (!HttpServerModule::authorize(req)) {
          res.status = 403;
          res.set_content("forbidden: cross-origin request blocked (missing X-Hades header)",
                          "text/plain");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;  // continue to routes
      });

  // Static web UI: serve `webroot` as the site root (GET / -> index.html, plus style.css/app.js).
  if (!srv.set_mount_point("/", webroot)) {
    std::cerr << "hades: warning: webroot directory not found: " << webroot
              << " — static web UI will not be served (JSON API still works)\n";
  }

  srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"ok":true})", "application/json");
  });
  srv.Get("/history", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(history_json().dump(), "application/json");
  });
  srv.Post("/chat", [this](const httplib::Request& req, httplib::Response& res) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    std::string msg = body.is_object() ? body.value("message", "") : "";
    res.set_content(handle_message(msg).dump(), "application/json");
  });
  srv.Post("/confirm", [this](const httplib::Request& req, httplib::Response& res) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    std::string id = body.is_object() ? body.value("id", "") : "";
    bool approved = body.is_object() && body.value("approved", false);
    res.set_content(handle_confirm(id, approved).dump(), "application/json");
  });
  std::cout << "hades serving on http://" << host << ":" << port << "/  (web UI + POST /chat)"
            << std::endl;
  srv.listen(host, port);
}
}  // namespace hades
