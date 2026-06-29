// include/hades/module/memory_module.h — dynamic memory retrieval app (MOOS-app style)
//
// Subscribes USER_MESSAGE; each turn it loads the JSONL store, ranks records by keyword
// overlap with the message, renders the top_n as a bullet block, and posts
// RETRIEVED_MEMORY for the Arbiter to inject. Save is handled separately by the
// save_memory tool (this module is read-only over the store). Empty result -> "".
#pragma once
#include <cstddef>
#include <string>
#include "hades/module.h"
namespace hades {
class Blackboard;
class MemoryModule : public Module {
public:
  std::string type() const override { return "memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

private:
  std::string store_path_ = ".hades/memory.jsonl";
  std::size_t top_n_ = 5;
  Blackboard* bb_ = nullptr;
};
}  // namespace hades
