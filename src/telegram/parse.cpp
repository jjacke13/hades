// src/telegram/parse.cpp — tolerant getUpdates parse + reply chunking + request builders
#include "hades/telegram/parse.h"
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
  if (j.is_discarded() || !j.is_object() || !j.value("ok", false)) return out;
  auto res = j.find("result");
  if (res == j.end() || !res->is_array()) return out;
  out.ok = true;
  for (const auto& u : *res) {
    if (!u.is_object()) continue;
    TgUpdate t;
    t.update_id = num(u, "update_id");
    if (t.update_id == 0) continue;
    if (auto m = u.find("message"); m != u.end() && m->is_object()) {
      auto txt = m->find("text");
      if (txt == m->end() || !txt->is_string()) continue;      // photos/stickers etc: skip
      if (!m->contains("from") || !(*m)["from"].is_object()) continue;
      if (!m->contains("chat") || !(*m)["chat"].is_object()) continue;
      t.kind = "message";
      t.from_id = num((*m)["from"], "id");
      t.chat_id = num((*m)["chat"], "id");
      t.text = txt->get<std::string>();
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
