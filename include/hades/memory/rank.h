// include/hades/memory/rank.h — archival retrieval: supersession + blended scoring
//
// Two stages. (1) ADMISSION: a record must share at least one token with the query, and
// among records sharing a non-empty `topic` only the newest survives (supersession — the
// stale value stops surfacing). (2) SCORING: survivors are ordered by relevance SCALED by a
// recency+reinforcement multiplier. Recency/reinforcement only reorder RELEVANT records;
// they never admit an irrelevant one, or a merely-recent fact would flood the memory block.
//
//   score = relevance * (1 + kRecencyWeight*recency + kReinforcementWeight*reinforcement)
//
// The boosts MULTIPLY rather than add, because relevance is a ratio (matched/|query|) and the
// live caller passes the whole user message: with a fixed additive bonus, one incidental match
// in a fresh record would outrank four substantive matches in an old one as soon as the query
// had >=3 distinct tokens. As a multiplier they are scale-invariant in |query| and bounded, so
// a record cannot be overtaken by anything worth (1+0.3+0.2)x less relevance.
//
// Pure: no files, no Blackboard, no clock (the 4-arg overload takes `now`) — this is the
// seam a v2 embeddings scorer slots in behind.
//
// KNOWN EDGE (by design): a topic whose NEWEST record is terse ("window") can become
// unretrievable for a query the older record would have matched — the old value is
// suppressed and the new one shares no token. Nothing beats stale here, but it means a
// topic-tagged correction must be SELF-CONTAINED (see prompts/soul.md).
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {

// Blend weights + decay. The tuning seam: change here, no manifest key in v1. The two boost
// weights are multiplier terms (see the formula above), so their sum bounds how much a fresh
// or often-restated record can make up for being less relevant: at most 1.5x here.
inline constexpr double kRelevanceWeight     = 1.0;
inline constexpr double kRecencyWeight       = 0.3;
inline constexpr double kReinforcementWeight = 0.2;
inline constexpr double kRecencyHalfLifeDays = 30.0;   // recency = 0.5^(age_days/half_life)

// `now` = reference epoch seconds for the recency term (test seam: deterministic scoring).
[[nodiscard]] std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                                      const std::string& query,
                                                      std::size_t top_n, double now);

// Clock-backed convenience overload (what the MemoryModule calls).
[[nodiscard]] std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                                      const std::string& query,
                                                      std::size_t top_n);
}  // namespace hades
