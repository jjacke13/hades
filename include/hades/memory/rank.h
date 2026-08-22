// include/hades/memory/rank.h — archival retrieval: supersession + blended scoring
//
// Two stages. (1) ADMISSION: a record must share at least one token with the query, and
// among records sharing a non-empty `topic` only the newest survives (supersession — the
// stale value stops surfacing). (2) SCORING: survivors are ordered by a weighted blend of
// relevance, recency and reinforcement. Recency/reinforcement only reorder RELEVANT records;
// they never admit an irrelevant one, or a merely-recent fact would flood the memory block.
// Pure: no files, no Blackboard, no clock (the 4-arg overload takes `now`) — this is the
// seam a v2 embeddings scorer slots in behind.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {

// Blend weights + decay. The tuning seam: change here, no manifest key in v1. Relevance is
// deliberately dominant so a strong old match still beats a weak fresh one.
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
