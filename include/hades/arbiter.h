// include/hades/arbiter.h — the "helm"; drives the per-turn agent decision loop
//
// Arbiter subscribes USER_MESSAGE -> posts LLM_REQUEST; on LLM_RESPONSE it
// constructs an Action, runs it through each Objective's veto(), then either
// posts TOOL_REQUEST / ASSISTANT_MESSAGE or a CONFIRM_REQUEST (soft veto).
// Tool results loop back via TOOL_RESULT; the turn ends when the LLM emits an
// Answer or the max-steps guard fires. Event-driven via the Blackboard; no threads.

#pragma once
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/entry.h"
#include "hades/module.h"
#include "hades/objective.h"
#include "hades/llm/provider.h"   // ToolSpec
namespace hades {
class Blackboard;
class Arbiter : public Module {
public:
  std::string type() const override { return "arbiter"; }
  void on_attach(Blackboard& bb) override;
  void add_objective(std::unique_ptr<Objective> o) { objectives_.push_back(std::move(o)); }
  void set_tools(std::vector<ToolSpec> t) { tools_ = std::move(t); }
  void set_model(std::string m) { model_ = std::move(m); }
  // Assembled system prompt (SOUL/USER/MEMORY); prepended as messages[0] each turn.
  void set_system_prompt(std::string s) { system_prompt_ = std::move(s); }

private:
  void start_turn();
  void on_llm_response(const Entry&);
  void on_tool_result(const Entry&);
  void on_confirm(const Entry&);
  void dispatch_or_gate(const Action&, const nlohmann::json& assistant_msg);

  Blackboard* bb_ = nullptr;
  std::vector<nlohmann::json> history_;
  std::vector<std::unique_ptr<Objective>> objectives_;
  std::vector<ToolSpec> tools_;
  std::string model_;
  std::string system_prompt_;   // prepended as a {role:system} message each turn (may be empty)
  // single pending confirm slot; the turn is suspended until it resolves (no second pending can form).
  nlohmann::json pending_;      // action awaiting confirm
  nlohmann::json pending_msg_;  // assistant tool_calls msg awaiting confirm
  int steps_ = 0;               // tool-call steps within the current turn (reset on USER_MESSAGE)
};
}  // namespace hades
