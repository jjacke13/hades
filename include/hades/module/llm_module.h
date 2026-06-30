// include/hades/module/llm_module.h — Module: LLM_REQUEST -> LLM_RESPONSE
//
// LLMModule subscribes to LLM_REQUEST on the Blackboard, calls
// Provider::complete(), and posts LLM_RESPONSE plus a cumulative
// BUDGET_SPENT_USD entry; the Provider is injected for testability.

#pragma once
#include <memory>
#include "hades/module.h"
#include "hades/llm/provider.h"
#include "hades/timeouts.h"   // kDefaultLlmTimeoutS
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
  // this module and the Blackboard are destroyed — the worker reads provider_
  // (owned here) and calls bb_->post(), so a front-end's run_until() observing
  // LLM_RESPONSE does NOT prove the worker has returned from the task. Teardown
  // order is load-bearing. (The worker no longer mutates spent_ — budget is
  // accrued on the pump thread by the LLM_RESPONSE handler.)
  void set_executor(Executor* e) { executor_ = e; }
  // The per-call cpr HTTP timeout (seconds) resolved from the Session block in
  // on_start (`llm_timeout_s`, default kDefaultLlmTimeoutS). Exposed so wiring can
  // enforce the idle>llm invariant and tests can assert the resolution.
  double resolved_llm_timeout_s() const { return llm_timeout_s_; }
private:
  std::unique_ptr<Provider> provider_;
  // Resolved in on_start BEFORE the provider build / api-key check, then passed to
  // cpr_http(); defaults to kDefaultLlmTimeoutS when `llm_timeout_s` is absent.
  double llm_timeout_s_ = kDefaultLlmTimeoutS;
  // spent_ is written ONLY on the pump thread (the LLM_RESPONSE handler accrues
  // it post-a-delta) → race-free regardless of overlapping offloaded LLM calls;
  // the worker mutates no module state.
  double price_per_mtok_ = 0.0, spent_ = 0.0;
  Blackboard* bb_ = nullptr;
  // Non-owning. MUST be destroyed (joins its workers) BEFORE this module/bb_ —
  // see set_executor(): the worker reads provider_/bb_ after posting LLM_RESPONSE.
  Executor* executor_ = nullptr;
};
}  // namespace hades
