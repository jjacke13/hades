// src/memory/rank.cpp — v1 keyword ranker (exact lowercased token overlap)
#include "hades/memory/rank.h"
#include <algorithm>
#include <cctype>
#include <set>
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
  struct Scored { const MemoryRecord* rec; int score; };
  std::vector<Scored> scored;
  for (const auto& rec : all) {
    const auto t = tokenize(rec.text);
    int score = 0;
    for (const auto& w : q) if (t.count(w)) ++score;
    if (score > 0) scored.push_back({&rec, score});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.rec->ts > b.rec->ts;
  });
  std::vector<MemoryRecord> out;
  for (std::size_t i = 0; i < scored.size() && i < top_n; ++i) out.push_back(*scored[i].rec);
  return out;
}

}  // namespace hades
