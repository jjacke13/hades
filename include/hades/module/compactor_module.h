// include/hades/module/compactor_module.h — background session summarizer (compaction)
//
// Answers the Arbiter's COMPACT_REQUEST with an aux LLM call that MERGES the existing
// rolling summary with the newly dropped turns (auto-extract discipline: own provider from
// the merged cfg, Executor worker, one in flight, AUX_SPENT_USD delta). The worker is
// throw-wrapped and ALWAYS posts a terminal reply — SESSION_SUMMARY on success,
// COMPACT_FAILED on any error — so the Arbiter's pending flag can never wedge (the
// tool-offload BG_DONE lesson). Fail-soft: no module in the roster -> no subscriber ->
// silent-truncation behavior unchanged.
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class Executor;

class CompactorModule : public Module {
public:
  explicit CompactorModule(std::unique_ptr<Provider> p = nullptr) : provider_(std::move(p)) {}
  std::string type() const override { return "compactor"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* e) { executor_ = e; }

private:
  std::unique_ptr<Provider> provider_;   // injected (tests) or built in on_start
  Executor* executor_ = nullptr;         // nullptr -> summarize runs inline (tests)
  Blackboard* bb_ = nullptr;
  std::string model_;
  double price_per_mtok_ = 0.0;
  std::size_t summary_char_limit_ = 4000;
  std::atomic<bool> busy_{false};        // one summarize in flight; worker clears it
};
}  // namespace hades
