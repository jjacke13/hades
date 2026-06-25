#pragma once
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {
struct ToolSpec { std::string name; std::string description; nlohmann::json schema; };
struct LlmRequest {
  std::vector<nlohmann::json> messages;   // [{role, content}|{role:assistant,tool_calls}|{role:tool,...}]
  std::vector<ToolSpec>       tools;
  std::string                 model;
};
struct LlmResponse {
  std::string                   text;       // "" if pure tool call
  std::optional<nlohmann::json> tool_call;  // {id, name, arguments(object)}
  int prompt_tokens = 0, completion_tokens = 0;
  std::string stop_reason;
};
class Provider {
public:
  virtual ~Provider() = default;
  virtual LlmResponse complete(const LlmRequest&) = 0;
};
}  // namespace hades
