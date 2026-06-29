// src/module/memory_module.cpp — load store, rank vs user message, post RETRIEVED_MEMORY
#include "hades/module/memory_module.h"
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/memory/rank.h"
#include "hades/memory/store.h"
namespace hades {

void MemoryModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("store")) store_path_ = cfg.kv.at("store");
  if (cfg.kv.count("top_n")) {
    try {
      long n = std::stol(cfg.kv.at("top_n"));
      if (n > 0) top_n_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default on garbage */ }
  }
}

void MemoryModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string()) return;  // ignore malformed input
    const auto all = load_memories(store_path_);
    const auto top = rank_memories(all, e.value.get<std::string>(), top_n_);
    std::string block;
    for (const auto& r : top) block += "- " + r.text + "\n";
    if (!block.empty() && block.back() == '\n') block.pop_back();
    bb_->post("RETRIEVED_MEMORY", block, "memory");  // "" when nothing matched
  });
}

}  // namespace hades
