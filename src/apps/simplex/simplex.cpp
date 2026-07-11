// src/apps/simplex/simplex.cpp — the SimpleX front-end app: event parse + WsSimplexApi + module
//
// parse_simplex_events: tolerant translation of daemon frames into SxEvents (canned-JSON tested).
// WsSimplexApi: the real SimplexApi over WsClient — corrId round-trips for commands, queuing any
// events that interleave; next_event pops the queue first. SimplexModule (Task 4): the front-end.
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
#include "hades/simplex/ws.h"

namespace hades {
namespace {
// {"resp": X}; X may be {"Right": Y}/{"Left": Y} (Haskell Either encoding in some CLI builds).
nlohmann::json unwrap_resp(const nlohmann::json& frame) {
  if (!frame.is_object()) return nullptr;
  auto it = frame.find("resp");
  if (it == frame.end() || !it->is_object()) return nullptr;
  nlohmann::json r = *it;
  if (r.contains("Right") && r["Right"].is_object()) r = r["Right"];
  else if (r.contains("Left") && r["Left"].is_object()) r = r["Left"];
  return r;
}
long long num(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : 0;
}
std::string str(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
}  // namespace

std::vector<SxEvent> parse_simplex_events(const std::string& frame_json) {
  std::vector<SxEvent> out;
  auto frame = nlohmann::json::parse(frame_json, nullptr, false);
  if (frame.is_discarded()) return out;
  const nlohmann::json resp = unwrap_resp(frame);
  if (!resp.is_object()) return out;
  const std::string type = str(resp, "type");

  if (type == "newChatItems") {
    auto items = resp.find("chatItems");
    if (items == resp.end() || !items->is_array()) return out;
    for (const auto& it : *items) {
      if (!it.is_object()) continue;
      const auto& ci = it.value("chatInfo", nlohmann::json::object());
      const auto& item = it.value("chatItem", nlohmann::json::object());
      if (str(ci, "type") != "direct") continue;                        // v1: DMs only
      const auto& contact = ci.value("contact", nlohmann::json::object());
      const auto& dir = item.value("chatDir", nlohmann::json::object());
      if (str(dir, "type") != "directRcv") continue;                    // skip our own echoes
      const auto& content = item.value("content", nlohmann::json::object());
      if (str(content, "type") != "rcvMsgContent") continue;
      const auto& mc = content.value("msgContent", nlohmann::json::object());
      if (str(mc, "type") != "text") continue;                          // v1: text only
      SxEvent ev;
      ev.kind = SxEvent::Kind::Text;
      ev.contact_id = num(contact, "contactId");
      ev.display_name = str(contact, "localDisplayName");
      ev.text = str(mc, "text");
      if (ev.contact_id != 0 && !ev.text.empty()) out.push_back(std::move(ev));
    }
  } else if (type == "receivedContactRequest") {
    const auto& cr = resp.value("contactRequest", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::ContactRequest;
    ev.request_id = num(cr, "contactRequestId");
    ev.display_name = str(cr, "localDisplayName");
    if (ev.request_id != 0) out.push_back(std::move(ev));
  } else if (type == "contactConnected") {
    const auto& c = resp.value("contact", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::Connected;
    ev.contact_id = num(c, "contactId");
    ev.display_name = str(c, "localDisplayName");
    if (ev.contact_id != 0) out.push_back(std::move(ev));
  }
  return out;
}

// ── WsSimplexApi: the real seam impl over WsClient (file-local; exposed via the factory) ─────
namespace {
constexpr double kCmdTimeoutS = 15.0;   // one command round-trip against the LOCAL daemon

class WsSimplexApi : public SimplexApi {
 public:
  WsSimplexApi(std::string host, int port, double connect_timeout_s)
      : host_(std::move(host)), port_(port), connect_timeout_s_(connect_timeout_s) {}

  bool reconnect() override {
    pending_.clear();
    return ws_.connect(host_, port_, connect_timeout_s_);
  }

  SxStatus next_event(double timeout_s, SxEvent& out) override {
    if (!pending_.empty()) {
      out = pending_.front();
      pending_.pop_front();
      return SxStatus::Event;
    }
    std::string frame;
    switch (ws_.recv_text(timeout_s, frame)) {
      case WsRecv::Timeout: return SxStatus::Timeout;
      case WsRecv::Closed: return SxStatus::Closed;
      case WsRecv::Error: return SxStatus::Error;
      case WsRecv::Text: break;
    }
    for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    if (pending_.empty()) return SxStatus::Timeout;   // frame parsed to nothing: caller re-loops
    out = pending_.front();
    pending_.pop_front();
    return SxStatus::Event;
  }

  bool send_text(long long contact_id, const std::string& text) override {
    nlohmann::json msgs = nlohmann::json::array(
        {{{"msgContent", {{"type", "text"}, {"text", text}}}}});
    return command_ok_("/_send @" + std::to_string(contact_id) + " json " + msgs.dump(),
                       "newChatItems");
  }

  bool accept_request(long long request_id) override {
    return command_ok_("/_accept " + std::to_string(request_id), "acceptingContactRequest");
  }

 private:
  // Send {corrId,cmd}; read until the matching resp (events seen meanwhile are queued).
  // ok iff the resp type equals ok_type. Total wait bounded by kCmdTimeoutS even across a
  // stream of non-matching frames. Timeout/close/error -> false (fail-soft; caller logs).
  bool command_ok_(const std::string& cmd, const char* ok_type) {
    if (!ws_.connected()) return false;
    const std::string corr = std::to_string(++corr_);
    nlohmann::json env{{"corrId", corr}, {"cmd", cmd}};
    if (!ws_.send_text(env.dump())) return false;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      const double remaining = kCmdTimeoutS - elapsed;
      if (remaining <= 0.0) return false;
      std::string frame;
      if (ws_.recv_text(remaining, frame) != WsRecv::Text) return false;
      auto j = nlohmann::json::parse(frame, nullptr, false);
      if (j.is_object() && j.value("corrId", "") == corr) {
        const nlohmann::json resp = unwrap_resp(j);
        return resp.is_object() && str(resp, "type") == ok_type;
      }
      for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    }
  }

  std::string host_;
  int port_;
  double connect_timeout_s_;
  WsClient ws_;
  long long corr_ = 0;
  std::deque<SxEvent> pending_;
};
}  // anonymous namespace

std::unique_ptr<SimplexApi> make_ws_simplex_api(std::string host, int port,
                                                double connect_timeout_s) {
  return std::make_unique<WsSimplexApi>(std::move(host), port, connect_timeout_s);
}
}  // namespace hades
