// include/hades/telegram/cpr_telegram_api.h — real Bot API transport over HTTPS (cpr)
//
// Thin, logic-free shell: URLs are https://api.telegram.org/bot<token>/<method>; bodies come
// from the tested build_* helpers; responses go through the tested parse_updates. The token is
// embedded in every URL — hades_main adds it to the Eventlog redaction, and errors logged here
// must never print the URL. Every failure path degrades (empty result / false), never throws.
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/telegram/api.h"
namespace hades {
class CprTelegramApi : public TelegramApi {
 public:
  explicit CprTelegramApi(std::string token);
  std::vector<TgUpdate> get_updates(long long offset, double timeout_s) override;
  bool send_message(long long chat_id, const std::string& text) override;
  bool send_confirm(long long chat_id, const std::string& prompt,
                    const std::string& confirm_id) override;
  void answer_callback(const std::string& callback_query_id) override;
  std::string get_file_path(const std::string& file_id) override;
  std::string download_file(const std::string& file_path) override;

 private:
  bool post_json_(const std::string& method, const nlohmann::json& body, double timeout_s);
  std::string base_;        // https://api.telegram.org/bot<token>
  std::string file_base_;   // https://api.telegram.org/file/bot<token>
};
}  // namespace hades
