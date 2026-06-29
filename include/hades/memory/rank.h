// include/hades/memory/rank.h — keyword retrieval over memory records (v1 scorer)
//
// Score each record by the number of distinct query tokens present in its text;
// drop zero-score records; sort by score desc, then ts desc (recency tie-break);
// return at most top_n. Pure: no files, no Blackboard. This is the seam a v2
// embeddings scorer slots into behind the same signature.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {
[[nodiscard]] std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                                    const std::string& query, std::size_t top_n);
}  // namespace hades
