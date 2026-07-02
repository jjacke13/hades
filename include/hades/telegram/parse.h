// include/hades/telegram/parse.h — pure Telegram Bot API parse/builder helpers
//
// parse_updates turns a getUpdates response body into typed TgUpdates (tolerant: malformed or
// non-text entries are skipped; a bad body -> ok=false; NEVER throws). split_message chunks a
// reply to Telegram's 4096-char message limit. The build_* helpers produce the exact JSON
// bodies for sendMessage / the inline-keyboard confirm / answerCallbackQuery, so the network
// layer (cpr) stays a thin, logic-free shell and tests pin the API shapes here.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {

struct TgUpdate {
  long long   update_id = 0;
  std::string kind;            // "message" | "callback"
  long long   from_id = 0;     // sender user id (allowlist check)
  long long   chat_id = 0;     // where to reply
  std::string text;            // message text (kind=="message")
  std::string callback_id;     // callback_query.id (kind=="callback")
  std::string callback_data;   // "approve:<id>" | "deny:<id>" (kind=="callback")
};

struct ParsedUpdates {
  std::vector<TgUpdate> updates;
  bool ok = false;             // false: body unparseable or Telegram replied ok!=true
};

ParsedUpdates parse_updates(const std::string& body);
std::vector<std::string> split_message(const std::string& text, std::size_t limit = 4096);
nlohmann::json build_send_message(long long chat_id, const std::string& text);
nlohmann::json build_confirm_message(long long chat_id, const std::string& prompt,
                                     const std::string& confirm_id);
nlohmann::json build_answer_callback(const std::string& callback_query_id);

}  // namespace hades
