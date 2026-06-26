#pragma once
#include <string>
#include <nlohmann/json.hpp>
namespace hades {
class Blackboard;
struct Action {
  enum class Kind { Answer, ToolCall, Wait };
  Kind kind = Kind::Wait;
  std::string    text;      // Answer
  std::string    tool;      // ToolCall name
  nlohmann::json args;      // ToolCall args object
  std::string    tool_id;   // ToolCall correlation id
};
struct VetoResult { bool vetoed = false; std::string reason; bool needs_confirm = false; };
class Objective {
public:
  virtual ~Objective() = default;
  virtual std::string type() const = 0;
  virtual bool        active(const Blackboard&) const { return true; }
  virtual VetoResult  veto(const Blackboard&, const Action&) const { return {}; }
};
}  // namespace hades
