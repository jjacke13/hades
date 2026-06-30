#include "hades/embedding/indexer.h"
#include <string>
#include <vector>
#include "hades/embedding/provider.h"
#include "hades/embedding/vector_cache.h"
#include "hades/memory/store.h"
namespace hades {
namespace {
// Embed a batch of (id,text) into the cache. Returns false on provider error (fail-soft stop).
bool flush_batch(EmbeddingProvider& provider, VectorCache& cache,
                 std::vector<std::pair<std::string, std::string>>& batch, IndexStats& st) {
  if (batch.empty()) return true;
  std::vector<std::string> texts;
  texts.reserve(batch.size());
  for (auto& [id, text] : batch) texts.push_back(text);
  EmbedResult r = provider.embed(texts);
  if (!r.error.empty() || r.vectors.size() != batch.size()) { st.ok = false; batch.clear(); return false; }
  for (std::size_t i = 0; i < batch.size(); ++i) {
    cache.put({batch[i].first, "memory", batch[i].second, r.vectors[i]});
    ++st.embedded;
  }
  batch.clear();
  return true;
}
}  // namespace

IndexStats index_archival(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& memory_store, std::size_t batch_size) {
  IndexStats st;
  const auto records = load_memories(memory_store);
  if (batch_size == 0) batch_size = 1;
  std::vector<std::pair<std::string, std::string>> batch;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const std::string id = "memory#" + std::to_string(i);
    if (cache.has(id)) { ++st.skipped; continue; }
    if (records[i].text.empty()) { ++st.skipped; continue; }
    batch.emplace_back(id, records[i].text);
    if (batch.size() >= batch_size && !flush_batch(provider, cache, batch, st)) return st;
  }
  flush_batch(provider, cache, batch, st);
  return st;
}
}  // namespace hades
