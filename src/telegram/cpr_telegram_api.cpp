// src/telegram/cpr_telegram_api.cpp — cpr glue for the Telegram Bot API (fail-soft, no logic)
#include "hades/telegram/cpr_telegram_api.h"
#include <cpr/cpr.h>
#include <iostream>
namespace hades {
namespace {
constexpr double kSendTimeoutS = 30.0;   // sendMessage/answerCallbackQuery are quick calls
}

CprTelegramApi::CprTelegramApi(std::string token)
    : base_("https://api.telegram.org/bot" + std::move(token)) {}

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
}  // namespace hades
