#include "hades/embedding/session_turns.h"
#include <filesystem>
#include "hades/session_history.h"
namespace hades {
// Safe role read: a non-string "role" (corrupt/external line) must NOT throw type_error (the
// function's never-throws contract) — treat anything but a string role as "" (no match).
static std::string role_of(const nlohmann::json& m) {
  auto it = m.find("role");
  return (it != m.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
std::vector<SessionTurn> extract_session_turns(const std::string& session_file) {
  std::vector<SessionTurn> turns;
  const auto msgs = read_session_jsonl(session_file);
  const std::string base = std::filesystem::path(session_file).filename().string();
  std::size_t idx = 0;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (role_of(msgs[i]) != "user") continue;
    if (!msgs[i].contains("content") || !msgs[i]["content"].is_string()) continue;
    const std::string user = msgs[i]["content"].get<std::string>();
    // find the NEXT assistant message with string content (fold tool / tool_call assistants)
    std::string answer;
    for (std::size_t j = i + 1; j < msgs.size(); ++j) {
      if (role_of(msgs[j]) == "user") break;             // next turn started, no answer
      if (role_of(msgs[j]) == "assistant" && msgs[j].contains("content") && msgs[j]["content"].is_string()) {
        answer = msgs[j]["content"].get<std::string>(); break;
      }
    }
    if (answer.empty()) continue;                                  // unanswered user -> drop
    turns.push_back({"session:" + base + "#" + std::to_string(idx), "U: " + user + "\nA: " + answer});
    ++idx;
  }
  return turns;
}
}  // namespace hades
