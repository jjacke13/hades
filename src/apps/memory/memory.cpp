// src/apps/memory/memory.cpp — the archival-memory app: module + keyword rank + store
//
// Merged (2026-07-04 src reorg): module/memory_module (RETRIEVED_MEMORY per turn) +
// memory/rank (pure keyword ranking; the seam embeddings plug behind) + memory/store
// (append-only jsonl).

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include "hades/module/memory_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/memory/rank.h"
#include "hades/memory/store.h"

// ── MemoryModule: load store, rank vs USER_MESSAGE, post RETRIEVED_MEMORY (was src/module/memory_module.cpp) ──────────────
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
    std::string rendered;
    for (const auto& r : top) rendered += "- " + r.text + "\n";
    if (!rendered.empty() && rendered.back() == '\n') rendered.pop_back();
    bb_->post("RETRIEVED_MEMORY", rendered, "memory");  // "" when nothing matched
  });
}

}  // namespace hades

// ── rank_memories: v1 keyword ranker (exact lowercased token overlap) (was src/memory/rank.cpp) ──────────────
namespace hades {

static std::set<std::string> tokenize(const std::string& s) {
  std::set<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!cur.empty()) { out.insert(cur); cur.clear(); }
  }
  if (!cur.empty()) out.insert(cur);
  return out;
}

std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n) {
  const auto q = tokenize(query);
  struct Scored { std::size_t idx; int score; };
  std::vector<Scored> scored;
  for (std::size_t i = 0; i < all.size(); ++i) {
    const auto t = tokenize(all[i].text);
    int score = 0;
    for (const auto& w : q) if (t.count(w)) ++score;
    if (score > 0) scored.push_back({i, score});
  }
  std::sort(scored.begin(), scored.end(), [&all](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return all[a.idx].ts > all[b.idx].ts;
  });
  std::vector<MemoryRecord> out;
  for (std::size_t i = 0; i < scored.size() && i < top_n; ++i) out.push_back(all[scored[i].idx]);
  return out;
}

}  // namespace hades

// ── load_memories: tolerant JSONL store reader (was src/memory/store.cpp) ──────────────
namespace hades {

std::vector<MemoryRecord> load_memories(const std::string& path) {
  std::vector<MemoryRecord> out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh agent, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("text") || !j["text"].is_string())
      continue;  // skip malformed / text-less records
    double ts = (j.contains("ts") && j["ts"].is_number()) ? j["ts"].get<double>() : 0.0;
    out.push_back({j["text"].get<std::string>(), ts});
  }
  return out;
}

}  // namespace hades
