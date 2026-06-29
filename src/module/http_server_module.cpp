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

namespace {
// Auth seam — the single chokepoint for adding authentication later. Today it
// allows every request; implementing auth = check a token/header here.
bool authorize(const httplib::Request& /*req*/) { return true; }
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

nlohmann::json HttpServerModule::collect_() {
  bb_->pump();  // run the turn to quiescence
  if (got_reply_) return {{"reply", last_reply_}};
  if (!pending_confirm_.is_null())
    return {{"needs_confirm", true},
            {"id", pending_confirm_.value("id", "")},
            {"prompt", pending_confirm_.value("prompt", "")}};
  return {{"reply", ""}};  // nothing to say
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

  // Auth chokepoint: runs before routing; a future token check goes in authorize().
  srv.set_pre_routing_handler(
      [](const httplib::Request& req, httplib::Response& res) {
        if (!authorize(req)) {
          res.status = 401;
          res.set_content("unauthorized", "text/plain");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;  // continue to routes
      });

  // Static web UI: serve `webroot` as the site root (GET / -> index.html, plus style.css/app.js).
  srv.set_mount_point("/", webroot);

  srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"ok":true})", "application/json");
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
