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

// ── SimplexModule: event loop, allowlist, turn driving, text y/N confirms ─────────────────────
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include "hades/module/simplex_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/telegram/parse.h"    // split_message
#include "hades/timeouts.h"          // kDefaultTurnIdleTimeoutS

namespace hades {
namespace {
constexpr std::size_t kSimplexSplit = 4000;

std::string trim(const std::string& s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(s.begin(), s.end(), ns);
  auto e = std::find_if(s.rbegin(), s.rend(), ns).base();
  return (b < e) ? std::string(b, e) : std::string{};
}
bool is_yes(const std::string& raw) {
  std::string t = trim(raw);
  std::transform(t.begin(), t.end(), t.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return t == "y" || t == "yes";
}
// Numeric token (all digits) -> id; anything else -> display name. Comma-separated, trimmed.
void split_contacts(const std::string& raw, std::set<long long>& ids, std::set<std::string>& names) {
  std::stringstream ss(raw);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    if (std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c); }))
      ids.insert(std::stoll(tok));
    else
      names.insert(tok);
  }
}
}  // namespace

SimplexModule::SimplexModule(std::unique_ptr<SimplexApi> api) : api_(std::move(api)) {}

SimplexModule::~SimplexModule() {
  stop_.store(true);
  stop_cv_.notify_all();
  // NOTE: a live next_event can hold the join up to ~poll_timeout_s_ (the WS read deadline).
  if (ev_thread_.joinable()) ev_thread_.join();
}

double SimplexModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void SimplexModule::on_start(const Block& cfg, Blackboard&) {
  // allow_contacts is REQUIRED — an open bot means anyone who connects can drive the agent's
  // tools (telegram allow_users precedent). Fail fast and loud.
  if (!cfg.kv.count("allow_contacts"))
    throw MalConfig("simplex module requires allow_contacts (contact ids and/or display names)");
  split_contacts(cfg.kv.at("allow_contacts"), allow_ids_, allow_names_);
  if (allow_ids_.empty() && allow_names_.empty())
    throw MalConfig("simplex module requires a non-empty allow_contacts");
  if (cfg.kv.count("auto_accept")) set_bool_on_string(cfg.kv.at("auto_accept"), auto_accept_);
  if (cfg.kv.count("notify_contact")) notify_contact_ = trim(cfg.kv.at("notify_contact"));
  if (cfg.kv.count("connect_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("connect_timeout_s"), t)) connect_timeout_s_ = t;
  }
  if (api_) return;                               // injected (tests)
  const std::string host = cfg.kv.count("host") ? cfg.kv.at("host") : "127.0.0.1";
  int port = 5225;
  if (cfg.kv.count("port")) {
    try { port = std::stoi(cfg.kv.at("port")); } catch (...) { port = 5225; }
  }
  api_ = make_ws_simplex_api(host, port, connect_timeout_s_);
}

void SimplexModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    pending_confirm_ = e.value;
  });
  // Notify sink (heartbeat etc.): deliver to the configured contact. Name form resolves via
  // known_ids_ (learned from Connected/Text events); unresolved -> logged skip. Fail-soft.
  bb.subscribe("NOTIFY_USER", [this](const Entry& e) {
    if (!api_ || notify_contact_.empty()) return;
    std::string text;
    if (e.value.is_object()) text = e.value.value("text", "");
    else if (e.value.is_string()) text = e.value.get<std::string>();
    if (text.empty()) return;
    long long cid = 0;
    if (std::all_of(notify_contact_.begin(), notify_contact_.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
      cid = std::stoll(notify_contact_);
    } else {
      auto it = known_ids_.find(notify_contact_);
      if (it != known_ids_.end()) cid = it->second;
    }
    if (cid == 0) {
      std::cerr << "hades: simplex notify skipped (contact not yet known: " << notify_contact_ << ")\n";
      return;
    }
    try {
      if (!api_->send_text(cid, text))
        std::cerr << "hades: simplex notify send failed (contact " << cid << ")\n";
    } catch (...) { /* fail-soft */ }
  });
}

bool SimplexModule::allowed_(const SxEvent& ev) const {
  return allow_ids_.count(ev.contact_id) || allow_names_.count(ev.display_name);
}

void SimplexModule::send_reply_(long long contact_id, const std::string& text) {
  for (const auto& chunk : split_message(text, kSimplexSplit))
    if (!api_->send_text(contact_id, chunk))
      std::cerr << "hades: simplex send failed (reply dropped; history is persisted)\n";
}

void SimplexModule::drive_turn_(long long contact_id, const nlohmann::json& post_value,
                                const char* key) {
  std::lock_guard<std::mutex> lk(turn_mu_());
  my_turn_ = true;
  struct Reset { bool& mine; ~Reset() { mine = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post("TURN_ORIGIN", "human", "simplex");
  bb_->post(key, post_value, "simplex");
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "simplex");
    bb_->pump();
    send_reply_(contact_id, "[timed out]");
    return;
  }
  if (got_reply_) {
    send_reply_(contact_id, last_reply_);
    return;
  }
  // Confirm-gated: send a y/N text prompt; the next message from this contact answers it.
  const std::string id = pending_confirm_.value("id", "");
  const std::string prompt = pending_confirm_.value("prompt", "");
  outstanding_confirm_id_ = id;
  outstanding_contact_id_ = contact_id;
  send_reply_(contact_id, (prompt.empty() ? std::string("confirm?") : prompt) +
                              "\n(reply y to approve, anything else to deny)");
}

void SimplexModule::handle_text_(const SxEvent& ev) {
  // An outstanding confirm is answered by the NEXT message from the SAME contact.
  if (!outstanding_confirm_id_.empty() && ev.contact_id == outstanding_contact_id_) {
    const std::string id = outstanding_confirm_id_;
    outstanding_confirm_id_.clear();
    drive_turn_(ev.contact_id, nlohmann::json{{"id", id}, {"approved", is_yes(ev.text)}},
                "CONFIRM_RESPONSE");
    return;
  }
  drive_turn_(ev.contact_id, nlohmann::json(ev.text), "USER_MESSAGE");
}

void SimplexModule::handle_event_(const SxEvent& ev) {
  // Learn name->id from any event carrying both (notify_contact name resolution).
  if (ev.contact_id != 0 && !ev.display_name.empty()) known_ids_[ev.display_name] = ev.contact_id;
  switch (ev.kind) {
    case SxEvent::Kind::Text:
      if (allowed_(ev)) handle_text_(ev);       // non-allowed: silently dropped
      break;
    case SxEvent::Kind::ContactRequest:
      if (auto_accept_) {
        if (!api_->accept_request(ev.request_id))
          std::cerr << "hades: simplex accept_request failed (" << ev.request_id << ")\n";
      } else {
        std::cerr << "hades: simplex contact request from \"" << ev.display_name
                  << "\" ignored (auto_accept=false; accept it in the simplex-chat CLI)\n";
      }
      break;
    case SxEvent::Kind::Connected:
      std::cerr << "hades: simplex contact connected: " << ev.display_name << "\n";
      break;
    case SxEvent::Kind::None:
      break;
  }
}

bool SimplexModule::step_once() {
  try {
    SxEvent ev;
    switch (api_->next_event(poll_timeout_s_, ev)) {
      case SxStatus::Event:
        handle_event_(ev);
        return true;
      case SxStatus::Timeout:
        return true;
      case SxStatus::Closed:
      case SxStatus::Error:
        return false;
    }
  } catch (const std::exception& e) {
    std::cerr << "hades: simplex event error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "hades: simplex event error (unknown)\n";
  }
  return true;
}

void SimplexModule::run_loop_() {
  // Initial connect + reconnect-on-drop, with an interruptible backoff.
  bool need_connect = true;
  while (!stop_.load()) {
    if (need_connect) {
      if (!api_->reconnect()) {
        // Backoff base = connect_timeout_s (spec); interruptible so the dtor never waits it out.
        std::cerr << "hades: simplex daemon unreachable; retrying\n";
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::duration<double>(connect_timeout_s_),
                          [this] { return stop_.load(); });
        continue;
      }
      need_connect = false;
    }
    if (!step_once()) need_connect = true;
  }
}

void SimplexModule::start() {
  if (ev_thread_.joinable()) return;   // idempotent
  ev_thread_ = std::thread([this] { run_loop_(); });
}

void SimplexModule::wait() {
  if (ev_thread_.joinable()) ev_thread_.join();
}
}  // namespace hades
