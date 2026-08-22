// src/apps/memory/memory.cpp — the archival-memory app: module + keyword rank + store
//
// Merged (2026-07-04 src reorg): module/memory_module (RETRIEVED_MEMORY per turn) +
// memory/rank (pure keyword ranking; the seam embeddings plug behind) + memory/store
// (append-only jsonl).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
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
                                        const std::string& query, std::size_t top_n,
                                        double now) {
  const auto q = tokenize(query);
  if (q.empty()) return {};   // no query tokens -> nothing is relevant
  // A non-finite `now` would make every score NaN, and a NaN-comparing predicate is not a
  // strict weak ordering -> UB inside std::sort. Fall back to "no recency information".
  if (!std::isfinite(now)) now = 0.0;

  // Reinforcement input: how many records restate each non-empty topic. Counted over the
  // WHOLE store (not just relevant records) — restating a fact reinforces it regardless of
  // how the current query happens to word things.
  std::map<std::string, std::size_t> bucket;
  for (const auto& r : all)
    if (!r.topic.empty()) ++bucket[r.topic];

  // Supersession: newest ts wins per non-empty topic. Ties on ts are broken by index so the
  // choice is deterministic (later line in an append-only store = later write).
  std::map<std::string, std::size_t> newest;   // topic -> winning index
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i].topic.empty()) continue;
    auto it = newest.find(all[i].topic);
    if (it == newest.end() || all[i].ts >= all[it->second].ts) newest[all[i].topic] = i;
  }

  struct Scored { std::size_t idx; double score; };
  std::vector<Scored> scored;
  for (std::size_t i = 0; i < all.size(); ++i) {
    // Admission 1: superseded records never surface. .at() not operator[]: the key is always
    // present (both maps are built over `all`), and a future refactor that filtered the map
    // would silently insert 0 here and crown record 0 as the survivor.
    if (!all[i].topic.empty() && newest.at(all[i].topic) != i) continue;
    // Admission 2: must be relevant to the query.
    const auto t = tokenize(all[i].text);
    std::size_t matched = 0;
    for (const auto& w : q) if (t.count(w)) ++matched;
    if (matched == 0) continue;

    const double relevance = static_cast<double>(matched) / static_cast<double>(q.size());
    // Same NaN defence as the `now` guard above, on the other operand: a non-finite ts would
    // make this record's score NaN, and a NaN score makes the comparator a non-strict-weak
    // ordering -> UB in std::sort. Unreachable from disk (the JSON parser discards non-finite
    // numbers) but the 4-arg overload is public and takes caller-built records.
    const double rec_ts = std::isfinite(all[i].ts) ? all[i].ts : 0.0;
    const double age_days = (now - rec_ts) / 86400.0;
    // Future/unknown timestamps clamp to full freshness / no boost rather than exploding.
    const double recency = age_days <= 0.0
                               ? 1.0
                               : std::pow(0.5, age_days / kRecencyHalfLifeDays);
    const std::size_t n = all[i].topic.empty() ? 1u : bucket.at(all[i].topic);
    const double reinforcement = 1.0 - 1.0 / (1.0 + static_cast<double>(n));

    // MULTIPLICATIVE, not additive. Relevance is a RATIO (matched/|query|), so an additive
    // bonus of a fixed 0.3 would outweigh a whole extra matched token as soon as the query
    // has >=3 distinct tokens — and the live caller passes the entire user message (10-40
    // tokens), where one incidental fresh match would beat four substantive old ones. As a
    // multiplier the boosts are scale-invariant in |query|: they reorder records WITHIN a
    // relevance band and can never overturn a record worth more than (1+0.3+0.2)x another.
    scored.push_back({i, kRelevanceWeight * relevance *
                             (1.0 + kRecencyWeight * recency +
                              kReinforcementWeight * reinforcement)});
  }

  // Fully ordered: score desc, then ts desc, then text asc — no ties left to chance.
  std::sort(scored.begin(), scored.end(), [&all](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    if (all[a.idx].ts != all[b.idx].ts) return all[a.idx].ts > all[b.idx].ts;
    return all[a.idx].text < all[b.idx].text;
  });
  std::vector<MemoryRecord> out;
  for (std::size_t i = 0; i < scored.size() && i < top_n; ++i) out.push_back(all[scored[i].idx]);
  return out;
}

std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n) {
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return rank_memories(all, query, top_n, now);
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
    std::string topic;   // absent or non-string -> "" (legacy line / junk): never throws
    // Normalize on READ too, not only in the writer: bucketing happens here, so canonicalizing
    // at this boundary means a hand-edited line ("Seat-Pref") or a future second writer cannot
    // land in a different bucket than save_memory's records for the same subject.
    if (j.contains("topic") && j["topic"].is_string())
      topic = normalize_topic(j["topic"].get<std::string>());
    out.push_back({j["text"].get<std::string>(), ts, topic});
  }
  return out;
}

}  // namespace hades
