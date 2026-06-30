// include/hades/embedding/defaults.h — single source of embedding default constants.
#pragma once
#include <cstddef>
namespace hades {
inline constexpr std::size_t kDefaultEmbedTopN = 5;
inline constexpr float       kDefaultMinSimilarity = 0.25f;   // weak-match floor
inline constexpr std::size_t kDefaultEmbedBatch = 32;
inline constexpr double      kDefaultEmbedTimeoutS = 120.0;
inline constexpr double      kDefaultReindexIntervalS = 86400.0;  // daily; 0 = launch-only
}  // namespace hades
