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
  // When unset, complete() runs inline on the pump thread (default, unchanged
  // behaviour).
  //
  // LIFETIME: Non-owning. The Executor MUST be joined/drained (destroyed) BEFORE
  // this module and the Blackboard are destroyed — the worker mutates spent_ and
  // posts BUDGET_SPENT_USD AFTER posting LLM_RESPONSE, so a front-end's
  // run_until() observing LLM_RESPONSE does NOT prove the worker has returned
  // from the task. Teardown order is load-bearing.
  void set_executor(Executor* e) { executor_ = e; }
private:
  std::unique_ptr<Provider> provider_;
  double price_per_mtok_ = 0.0, spent_ = 0.0;
  Blackboard* bb_ = nullptr;
  // Non-owning. MUST be destroyed (joins its workers) BEFORE this module/bb_ —
  // see set_executor(): the worker touches spent_/bb_ after posting LLM_RESPONSE.
  Executor* executor_ = nullptr;
};
}  // namespace hades
