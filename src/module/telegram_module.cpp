// src/module/telegram_module.cpp — poll loop, allowlist, turn driving, inline-keyboard confirms
#include "hades/module/telegram_module.h"
#include <chrono>
#include <exception>
#include <iostream>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"                       // MalConfig
#include "hades/telegram/cpr_telegram_api.h"
#include "hades/telegram/parse.h"                 // split_message
#include "hades/timeouts.h"                       // kDefaultTurnIdleTimeoutS
#include <cstdlib>
#include <sstream>
namespace hades {

TelegramModule::TelegramModule(std::unique_ptr<TelegramApi> api) : api_(std::move(api)) {}

TelegramModule::~TelegramModule() {
  stop_.store(true);
  stop_cv_.notify_all();
  // NOTE: a live get_updates long-poll can hold the join up to ~poll_timeout_s+10 (cpr cannot
  // be cancelled). Acceptable v1: Ctrl-C terminates the process; a /quit exit waits one poll.
  if (poll_thread_.joinable()) poll_thread_.join();
}

double TelegramModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void TelegramModule::on_start(const Block& cfg, Blackboard&) {
  // allow_users is REQUIRED and strictly numeric — an open bot means anyone who finds the
  // username can drive the agent's tools. Fail fast and loud (pin_fact precedent).
  if (!cfg.kv.count("allow_users"))
    throw MalConfig("telegram module requires allow_users (numeric Telegram user ids)");
  std::istringstream is(cfg.kv.at("allow_users"));
  std::string tok;
  while (is >> tok) {
    try {
      std::size_t pos = 0;
      long long id = std::stoll(tok, &pos);
      if (pos != tok.size()) throw std::invalid_argument(tok);
      allow_.insert(id);
    } catch (const std::exception&) {
      throw MalConfig("telegram allow_users: not a numeric user id: " + tok);
    }
  }
  if (allow_.empty())
    throw MalConfig("telegram module requires a non-empty allow_users");
  if (cfg.kv.count("poll_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("poll_timeout_s"), t)) poll_timeout_s_ = t;
  }
  if (api_) return;                               // injected (tests)
  const std::string env =
      cfg.kv.count("token_env") ? cfg.kv.at("token_env") : "TELEGRAM_BOT_TOKEN";
  const char* token = std::getenv(env.c_str());
  if (!token) throw MalConfig("telegram bot token env var not set: " + env);
  api_ = std::make_unique<CprTelegramApi>(token);
}

void TelegramModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Capture ONLY for turns this module drives (my_turn_) — a REPL/web turn's reply or confirm
  // is not ours to send to Telegram (turn-owner guard; symmetric to ChatModule's stdin guard).
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    pending_confirm_ = e.value;
  });
}

void TelegramModule::send_reply_(long long chat_id, const std::string& text) {
  for (const auto& chunk : split_message(text))
    if (!api_->send_message(chat_id, chunk))
      std::cerr << "hades: telegram sendMessage failed (reply dropped; history is persisted)\n";
}

// Shared turn driver for both entry points: lock the gate, reset capture, post the triggering
// event, run the turn to reply-or-confirm, then deliver the outcome to the chat.
void TelegramModule::drive_turn_(long long chat_id, const nlohmann::json& post_value,
                                 const char* key) {
  std::lock_guard<std::mutex> lk(turn_mu_());
  my_turn_ = true;
  // RAII reset declared AFTER the lock: runs BEFORE the mutex releases on EVERY exit path —
  // a handler throw propagating out of run_until must not leave my_turn_ true with the gate free.
  struct Reset { bool& f; ~Reset() { f = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post(key, post_value, "telegram");
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, effective_timeout_());
  if (!done) {
    // Idle timeout: abandon the turn (Arbiter bumps its epoch on TURN_ABANDONED) and say so.
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "telegram");
    bb_->pump();
    send_reply_(chat_id, "[timed out]");
    return;
  }
  if (got_reply_) {
    send_reply_(chat_id, last_reply_);
    return;
  }
  // Confirm-gated: send the inline keyboard and remember what we are waiting for. The
  // Arbiter's pending slot survives between turns (same contract as POST /confirm).
  const std::string id = pending_confirm_.value("id", "");
  const std::string prompt = pending_confirm_.value("prompt", "");
  outstanding_confirm_id_ = id;
  outstanding_chat_id_ = chat_id;
  if (!api_->send_confirm(chat_id, prompt.empty() ? "confirm?" : prompt, id))
    std::cerr << "hades: telegram send_confirm failed (confirm still pending in the agent)\n";
}

void TelegramModule::handle_text_(const TgUpdate& u) {
  drive_turn_(u.chat_id, nlohmann::json(u.text), "USER_MESSAGE");
}

void TelegramModule::handle_callback_(const TgUpdate& u) {
  api_->answer_callback(u.callback_id);           // always dismiss the client spinner
  const bool approve = u.callback_data.rfind("approve:", 0) == 0;
  const bool deny = u.callback_data.rfind("deny:", 0) == 0;
  if (!approve && !deny) return;
  const std::string id = u.callback_data.substr(u.callback_data.find(':') + 1);
  if (id.empty() || id != outstanding_confirm_id_) return;   // stale/unknown: dismissed only
  outstanding_confirm_id_.clear();
  drive_turn_(outstanding_chat_id_, nlohmann::json{{"id", id}, {"approved", approve}},
              "CONFIRM_RESPONSE");
}

bool TelegramModule::poll_once() {
  try {
    if (!drained_) {
      // Startup backlog: consume until empty and DISCARD — commands queued while the agent
      // was down (Telegram keeps updates 24h) must not replay against the live agent.
      for (;;) {
        auto stale = api_->get_updates(offset_, 0.0);
        if (stale.empty()) break;
        for (const auto& u : stale) offset_ = std::max(offset_, u.update_id + 1);
      }
      drained_ = true;
      return true;
    }
    auto updates = api_->get_updates(offset_, poll_timeout_s_);
    for (const auto& u : updates) {
      offset_ = std::max(offset_, u.update_id + 1);
      if (!allow_.count(u.from_id)) continue;      // silently drop non-allowed senders
      if (u.kind == "message" && !u.text.empty()) handle_text_(u);
      else if (u.kind == "callback") handle_callback_(u);
    }
    return true;
  } catch (const std::exception& e) {
    std::cerr << "hades: telegram poll error: " << e.what() << "\n";
    return false;
  } catch (...) {
    std::cerr << "hades: telegram poll error (unknown)\n";
    return false;
  }
}

void TelegramModule::run_loop_() {
  while (!stop_.load()) {
    const bool ok = poll_once();
    if (!ok && !stop_.load()) {
      // Backoff on error; interruptible so the dtor never waits the full 5s.
      std::unique_lock<std::mutex> lk(stop_mu_);
      stop_cv_.wait_for(lk, std::chrono::seconds(5), [this] { return stop_.load(); });
    }
  }
}

void TelegramModule::start_polling() {
  if (poll_thread_.joinable()) return;             // idempotent
  poll_thread_ = std::thread([this] { run_loop_(); });
}

void TelegramModule::wait() {
  if (poll_thread_.joinable()) poll_thread_.join();
}
}  // namespace hades
