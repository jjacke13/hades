// include/hades/module/auto_extract_module.h — post-turn background memory harvest
//
// Reviews each HUMAN turn's last exchange with an aux LLM call (own provider, built from the
// merged cfg block the wiring passes — Session values + AutoExtract overrides) and appends the
// parsed facts to the archival store with src:"auto". The call runs on the Executor when set
// (never blocks the pump thread); at most ONE review is in flight (a turn finishing mid-review
// is skipped, never queued). Peer/heartbeat turns are never harvested (memory-pollution /
// injection surface). Spend is posted as an AUX_SPENT_USD delta; the LLMModule folds it into
// the cumulative budget. Fail-soft everywhere: an extractor error can never touch a turn.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class Executor;

class AutoExtractModule : public Module {
public:
  explicit AutoExtractModule(std::unique_ptr<Provider> p = nullptr) : provider_(std::move(p)) {}
  std::string type() const override { return "auto_extract"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* e) { executor_ = e; }

private:
  std::unique_ptr<Provider> provider_;   // injected (tests) or built in on_start
  Executor* executor_ = nullptr;         // nullptr -> review runs inline (tests)
  Blackboard* bb_ = nullptr;
  std::string store_ = ".hades/memory.jsonl";
  std::string model_;
  double price_per_mtok_ = 0.0;
  std::size_t max_facts_ = 3;
  std::string origin_ = "human";         // latest TURN_ORIGIN (pump thread only)
  std::string last_user_;                // latest USER_MESSAGE (pump thread only)
  std::atomic<bool> busy_{false};        // one review in flight; worker clears it
};
}  // namespace hades
