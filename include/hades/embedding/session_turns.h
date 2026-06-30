// include/hades/embedding/session_turns.h — extract per-turn (user+assistant) units from a session jsonl
// for embedding. Each {role:user,content:string} pairs with the NEXT {role:assistant,content:string};
// intervening tool / tool_call-assistant messages fold out (a turn is one Q + its text answer). A
// trailing user with no following assistant answer is dropped. id = "session:<basename>#<turn-index>".
#pragma once
#include <string>
#include <vector>
namespace hades {
struct SessionTurn { std::string id; std::string text; };
std::vector<SessionTurn> extract_session_turns(const std::string& session_file);
}  // namespace hades
