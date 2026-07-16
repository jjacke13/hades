// include/hades/extract/extract.h — pure helpers behind the auto-extract module
//
// parse_extract_reply: the aux model's reply contract is "NONE" or a JSON array of short
// strings (a ```json fence is tolerated); everything else parses to no facts (fail-closed —
// a confused model must never write garbage memories). build_extract_digest: the one-exchange
// review input. is_turn_artifact: bracketed Arbiter/front-end outcomes are not conversation.
#pragma once
#include <string>
#include <vector>
namespace hades {
std::vector<std::string> parse_extract_reply(const std::string& reply, std::size_t max_facts);
std::string build_extract_digest(const std::string& user, const std::string& assistant);
bool is_turn_artifact(const std::string& assistant_text);
}  // namespace hades
