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
  // Voice input: resolve a file_id to a Bot-API file_path (getFile), then download the bytes.
  // Both return "" on any error (fail-soft; the module surfaces a text reply).
  virtual std::string get_file_path(const std::string& file_id) = 0;
  virtual std::string download_file(const std::string& file_path) = 0;
  // Send a voice note (ogg-opus bytes) via sendVoice. Returns false on failure (fail-soft; the
  // text reply has already been delivered — the voice is a best-effort bonus).
  virtual bool send_voice(long long chat_id, const std::string& ogg_bytes) = 0;
};
}  // namespace hades
