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
// Arbiter v1 ("the helm"): the per-turn agent loop. On USER_MESSAGE it asks
// the LLM (LLM_REQUEST); on LLM_RESPONSE it builds an Action and gates it
// through the active objectives (veto / confirm) before dispatching a tool
// call or surfacing an answer. Event-driven via the blackboard; no threads.
class Arbiter : public Module {
public:
  std::string type() const override { return "arbiter"; }
  void on_attach(Blackboard& bb) override;
  void add_objective(std::unique_ptr<Objective> o) { objectives_.push_back(std::move(o)); }
  void set_tools(std::vector<ToolSpec> t) { tools_ = std::move(t); }
  void set_model(std::string m) { model_ = std::move(m); }

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
  nlohmann::json pending_;      // action awaiting confirm
  nlohmann::json pending_msg_;  // assistant tool_calls msg awaiting confirm
};
}  // namespace hades
