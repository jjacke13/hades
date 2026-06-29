// include/hades/module/llm_module.h — Module: LLM_REQUEST -> LLM_RESPONSE
//
// LLMModule subscribes to LLM_REQUEST on the Blackboard, calls
// Provider::complete(), and posts LLM_RESPONSE plus a cumulative
// BUDGET_SPENT_USD entry; the Provider is injected for testability.

#pragma once
#include <memory>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class Executor;
class LLMModule : public Module {
public:
  LLMModule() = default;
  explicit LLMModule(std::unique_ptr<Provider> p);   // test injection
  std::string type() const override { return "llm"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  // Opt-in: when set, the blocking provider_->complete() call is offloaded to a
  // worker thread that posts LLM_RESPONSE/BUDGET_SPENT_USD back onto the bus.
  // Non-owning; the Executor must outlive this module. When unset, complete()
  // runs inline on the pump thread (default, unchanged behaviour).
  void set_executor(Executor* e) { executor_ = e; }
private:
  std::unique_ptr<Provider> provider_;
  double price_per_mtok_ = 0.0, spent_ = 0.0;
  Blackboard* bb_ = nullptr;
  Executor* executor_ = nullptr;   // non-owning, outlives this module
};
}  // namespace hades
