// include/hades/embedding/indexer.h — incremental embedding of a corpus into a VectorCache.
//
// index_archival: each memory.jsonl record i has stable id "memory#i". Only ids absent from the
// cache are embedded (in batch_size batches); a provider error stops early and returns ok=false
// with whatever was embedded (fail-soft — the module keeps running, keyword still answers).
#pragma once
#include <cstddef>
#include <string>
namespace hades {
class EmbeddingProvider;
class VectorCache;
struct IndexStats { std::size_t embedded = 0; std::size_t skipped = 0; bool ok = true; };
IndexStats index_archival(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& memory_store, std::size_t batch_size);
}  // namespace hades
