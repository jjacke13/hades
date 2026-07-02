// include/hades/telegram/api.h — Telegram Bot API seam (real impl: cpr; tests: scripted fake)
//
// The TelegramModule talks ONLY to this interface, so its whole turn/confirm/allowlist logic is
// testable without a network (the HttpClient-in-provider precedent). get_updates returns already-
// parsed updates ({} on any error — fail-soft); send_* return false on failure (module logs and
// carries on; the turn's history is already persisted by the Arbiter regardless).
#pragma once
#include <string>
#include <vector>
#include "hades/telegram/parse.h"  // TgUpdate
namespace hades {
class TelegramApi {
 public:
  virtual ~TelegramApi() = default;
  virtual std::vector<TgUpdate> get_updates(long long offset, double timeout_s) = 0;
  virtual bool send_message(long long chat_id, const std::string& text) = 0;
  virtual bool send_confirm(long long chat_id, const std::string& prompt,
                            const std::string& confirm_id) = 0;
  virtual void answer_callback(const std::string& callback_query_id) = 0;
};
}  // namespace hades
