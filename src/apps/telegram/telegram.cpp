// src/apps/telegram/telegram.cpp — the Telegram front-end app: module + parse + cpr shell
//
// Merged (2026-07-04 src reorg): module/telegram_module (long-poll, allowlist, DM-only,
// TurnGate turns, inline-keyboard confirms) + telegram/parse (tolerant Bot-API parse/
// builders, 4096 split) + telegram/cpr_telegram_api (thin network shell, method-only logging).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cpr/cpr.h>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include "hades/module/telegram_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"                       // MalConfig
#include "hades/stt/provider.h"                   // SttProvider / SttResult
#include "hades/telegram/cpr_telegram_api.h"
#include "hades/telegram/parse.h"                 // split_message
#include "hades/timeouts.h"                       // kDefaultTurnIdleTimeoutS

// ── TelegramModule: poll loop, allowlist, turn driving, inline-keyboard confirms (was src/module/telegram_module.cpp) ──────────────
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
  bb_->post("TURN_ORIGIN", "human", "telegram");
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

namespace {
// RAII temp file for a downloaded voice clip: unique name in the system temp dir, unlinked on scope
// exit (success OR error). The poll thread handles one voice at a time, so a per-call file is fine.
struct TempAudio {
  std::filesystem::path path;
  bool ok = false;
  explicit TempAudio(const std::string& bytes) {
    static std::atomic<unsigned long long> counter{0};
    const std::string name = "hades_stt_" + std::to_string(::getpid()) + "_" +
                             std::to_string(counter.fetch_add(1)) + ".oga";
    path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) {
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      ok = static_cast<bool>(f);
    }
  }
  ~TempAudio() { std::error_code ec; std::filesystem::remove(path, ec); }
  TempAudio(const TempAudio&) = delete;
  TempAudio& operator=(const TempAudio&) = delete;
};

std::string trim_ws(const std::string& s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(s.begin(), s.end(), ns);
  auto e = std::find_if(s.rbegin(), s.rend(), ns).base();
  return (b < e) ? std::string(b, e) : std::string{};
}
}  // namespace

// Voice update: download the clip, write it to a temp file, transcribe, then drive a normal turn
// with the transcript. Fail-soft — every failure path sends a text reply and posts NO USER_MESSAGE;
// the whole block is try/catch-guarded so no exception escapes the poll loop. Transcription happens
// BEFORE drive_turn_ locks the gate (drive_turn_ locks it itself, as it does for text).
void TelegramModule::handle_voice_(const TgUpdate& u) {
  if (!stt_) { send_reply_(u.chat_id, "voice input isn't enabled"); return; }
  try {
    const std::string fpath = api_->get_file_path(u.voice_file_id);
    if (fpath.empty()) { send_reply_(u.chat_id, "couldn't fetch your voice message"); return; }
    const std::string bytes = api_->download_file(fpath);
    if (bytes.empty()) { send_reply_(u.chat_id, "couldn't download your voice message"); return; }
    TempAudio tmp(bytes);
    if (!tmp.ok) { send_reply_(u.chat_id, "couldn't save your voice message"); return; }
    SttResult r = stt_->transcribe(tmp.path.string());
    if (!r.ok) {
      send_reply_(u.chat_id, "couldn't transcribe your voice message: " + r.error);
      return;
    }
    const std::string text = trim_ws(r.text);
    if (text.empty()) { send_reply_(u.chat_id, "didn't catch that"); return; }
    drive_turn_(u.chat_id, nlohmann::json(text), "USER_MESSAGE");
  } catch (const std::exception& e) {
    send_reply_(u.chat_id, std::string("voice error: ") + e.what());
  }
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
      // v1 is PRIVATE-CHAT-ONLY: in a group, non-allowlisted members could read replies and
      // see confirm buttons. A DM always has chat_id == from_id; anything else is dropped.
      if (u.kind == "message" && u.chat_id != u.from_id) continue;
      if (u.kind == "message" && !u.text.empty()) handle_text_(u);
      else if (u.kind == "message" && !u.voice_file_id.empty()) handle_voice_(u);
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

// ── parse_updates/split_message + request builders: tolerant Bot-API parse (was src/telegram/parse.cpp) ──────────────
namespace hades {
namespace {
// Type-safe numeric extraction: {"id":42} -> 42; missing/non-number -> 0 (entry then skipped).
long long num(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : 0;
}
}  // namespace

ParsedUpdates parse_updates(const std::string& body) {
  ParsedUpdates out;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return out;
  auto ok_it = j.find("ok");
  if (ok_it == j.end() || !ok_it->is_boolean() || !ok_it->get<bool>()) return out;
  auto res = j.find("result");
  if (res == j.end() || !res->is_array()) return out;
  out.ok = true;
  for (const auto& u : *res) {
    if (!u.is_object()) continue;
    TgUpdate t;
    t.update_id = num(u, "update_id");
    if (t.update_id == 0) continue;
    if (auto m = u.find("message"); m != u.end() && m->is_object()) {
      if (!m->contains("from") || !(*m)["from"].is_object()) continue;
      if (!m->contains("chat") || !(*m)["chat"].is_object()) continue;
      auto txt = m->find("text");
      if (txt != m->end() && txt->is_string()) {
        t.text = txt->get<std::string>();
      } else if (auto v = m->find("voice"); v != m->end() && v->is_object()) {
        auto fid = v->find("file_id");
        if (fid == v->end() || !fid->is_string()) continue;    // voice without a file_id: nothing to fetch
        t.voice_file_id = fid->get<std::string>();
      } else {
        continue;                                              // photos/stickers/etc: skip
      }
      t.kind = "message";
      t.from_id = num((*m)["from"], "id");
      t.chat_id = num((*m)["chat"], "id");
      if (t.from_id == 0 || t.chat_id == 0) continue;
      out.updates.push_back(std::move(t));
    } else if (auto c = u.find("callback_query"); c != u.end() && c->is_object()) {
      t.kind = "callback";
      if (auto id = c->find("id"); id != c->end() && id->is_string())
        t.callback_id = id->get<std::string>();
      if (auto d = c->find("data"); d != c->end() && d->is_string())
        t.callback_data = d->get<std::string>();
      if (c->contains("from") && (*c)["from"].is_object()) t.from_id = num((*c)["from"], "id");
      if (c->contains("message") && (*c)["message"].is_object() &&
          (*c)["message"].contains("chat") && (*c)["message"]["chat"].is_object())
        t.chat_id = num((*c)["message"]["chat"], "id");
      if (t.callback_id.empty() || t.from_id == 0) continue;
      out.updates.push_back(std::move(t));
    }
  }
  return out;
}

std::vector<std::string> split_message(const std::string& text, std::size_t limit) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < text.size(); i += limit) out.push_back(text.substr(i, limit));
  return out;
}

nlohmann::json build_send_message(long long chat_id, const std::string& text) {
  return {{"chat_id", chat_id}, {"text", text}};
}

nlohmann::json build_confirm_message(long long chat_id, const std::string& prompt,
                                     const std::string& confirm_id) {
  nlohmann::json row = nlohmann::json::array(
      {{{"text", "Approve"}, {"callback_data", "approve:" + confirm_id}},
       {{"text", "Deny"}, {"callback_data", "deny:" + confirm_id}}});
  return {{"chat_id", chat_id},
          {"text", prompt},
          {"reply_markup", {{"inline_keyboard", nlohmann::json::array({row})}}}};
}

nlohmann::json build_answer_callback(const std::string& callback_query_id) {
  return {{"callback_query_id", callback_query_id}};
}
}  // namespace hades

// ── CprTelegramApi: cpr glue for the Bot API (fail-soft, no logic) (was src/telegram/cpr_telegram_api.cpp) ──────────────
namespace hades {
namespace {
constexpr double kSendTimeoutS = 30.0;   // sendMessage/answerCallbackQuery are quick calls
}

CprTelegramApi::CprTelegramApi(std::string token)
    : base_("https://api.telegram.org/bot" + token),
      file_base_("https://api.telegram.org/file/bot" + std::move(token)) {}

std::vector<TgUpdate> CprTelegramApi::get_updates(long long offset, double timeout_s) {
  // Long-poll: Telegram holds the request up to timeout_s; the cpr cap sits above it so a
  // full-length poll is never cut off client-side. Errors -> {} (the caller backs off).
  nlohmann::json body{{"offset", offset}, {"timeout", static_cast<long long>(timeout_s)}};
  auto r = cpr::Post(cpr::Url{base_ + "/getUpdates"},
                     cpr::Header{{"Content-Type", "application/json"}},
                     cpr::Body{body.dump()},
                     cpr::Timeout{static_cast<int>((timeout_s + 10.0) * 1000)});
  if (r.status_code != 200) {
    std::cerr << "hades: telegram getUpdates failed (status " << r.status_code << ")\n";
    return {};
  }
  auto p = parse_updates(r.text);
  if (!p.ok) std::cerr << "hades: telegram getUpdates: unparseable response\n";
  return p.updates;
}

bool CprTelegramApi::post_json_(const std::string& method, const nlohmann::json& body,
                                double timeout_s) {
  auto r = cpr::Post(cpr::Url{base_ + "/" + method},
                     cpr::Header{{"Content-Type", "application/json"}},
                     cpr::Body{body.dump()},
                     cpr::Timeout{static_cast<int>(timeout_s * 1000)});
  if (r.status_code != 200) {
    // Log the METHOD only — the URL carries the bot token.
    std::cerr << "hades: telegram " << method << " failed (status " << r.status_code << ")\n";
    return false;
  }
  return true;
}

bool CprTelegramApi::send_message(long long chat_id, const std::string& text) {
  return post_json_("sendMessage", build_send_message(chat_id, text), kSendTimeoutS);
}

bool CprTelegramApi::send_confirm(long long chat_id, const std::string& prompt,
                                  const std::string& confirm_id) {
  return post_json_("sendMessage", build_confirm_message(chat_id, prompt, confirm_id),
                    kSendTimeoutS);
}

void CprTelegramApi::answer_callback(const std::string& callback_query_id) {
  post_json_("answerCallbackQuery", build_answer_callback(callback_query_id), kSendTimeoutS);
}

std::string CprTelegramApi::get_file_path(const std::string& file_id) {
  // getFile -> {"ok":true,"result":{"file_path":"voice/file_5.oga"}}. Errors -> "".
  auto r = cpr::Post(cpr::Url{base_ + "/getFile"},
                     cpr::Header{{"Content-Type", "application/json"}},
                     cpr::Body{nlohmann::json{{"file_id", file_id}}.dump()},
                     cpr::Timeout{static_cast<int>(kSendTimeoutS * 1000)}, cpr::Redirect{false});
  if (r.status_code != 200) {
    std::cerr << "hades: telegram getFile failed (status " << r.status_code << ")\n";
    return "";
  }
  auto j = nlohmann::json::parse(r.text, nullptr, false);
  if (!j.is_object() || !j.value("ok", false)) return "";
  auto res = j.find("result");
  if (res == j.end() || !res->is_object()) return "";
  auto fp = res->find("file_path");
  return (fp != res->end() && fp->is_string()) ? fp->get<std::string>() : "";
}

std::string CprTelegramApi::download_file(const std::string& file_path) {
  // GET https://api.telegram.org/file/bot<token>/<file_path> -> raw bytes ("" on error).
  auto r = cpr::Get(cpr::Url{file_base_ + "/" + file_path},
                    cpr::Timeout{static_cast<int>(kSendTimeoutS * 1000)}, cpr::Redirect{false});
  if (r.status_code != 200) {
    std::cerr << "hades: telegram file download failed (status " << r.status_code << ")\n";
    return "";
  }
  return r.text;
}
bool CprTelegramApi::send_voice(long long chat_id, const std::string& ogg_bytes) {
  // sendVoice as multipart with the bytes carried IN MEMORY (cpr::Buffer) — no temp file. Telegram
  // requires OGG/Opus for a voice note; the provider guarantees that. Errors -> false (text stands).
  auto r = cpr::Post(cpr::Url{base_ + "/sendVoice"},
                     cpr::Multipart{{"chat_id", std::to_string(chat_id)},
                                    {"voice", cpr::Buffer{ogg_bytes.begin(), ogg_bytes.end(),
                                                          "voice.ogg"}}},
                     cpr::Timeout{static_cast<int>(kSendTimeoutS * 1000)}, cpr::Redirect{false});
  if (r.status_code != 200) {
    std::cerr << "hades: telegram sendVoice failed (status " << r.status_code << ")\n";
    return false;
  }
  return true;
}
}  // namespace hades
