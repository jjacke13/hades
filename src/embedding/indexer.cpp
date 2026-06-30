#include "hades/embedding/indexer.h"
#include <filesystem>
#include <string>
#include <vector>
#include "hades/embedding/provider.h"
#include "hades/embedding/session_turns.h"
#include "hades/embedding/vector_cache.h"
#include "hades/memory/store.h"
namespace hades {
namespace {
// Embed a batch of (id,text) into the cache under source label `src` (e.g. "memory" / "session").
// Returns false on provider error (fail-soft stop).
bool flush_batch(EmbeddingProvider& provider, VectorCache& cache,
                 std::vector<std::pair<std::string, std::string>>& batch, IndexStats& st,
                 const std::string& src) {
  if (batch.empty()) return true;
  std::vector<std::string> texts;
  texts.reserve(batch.size());
  for (auto& [id, text] : batch) texts.push_back(text);
  EmbedResult r = provider.embed(texts);
  if (!r.error.empty() || r.vectors.size() != batch.size()) { st.ok = false; batch.clear(); return false; }
  for (std::size_t i = 0; i < batch.size(); ++i) {
    cache.put({batch[i].first, src, batch[i].second, r.vectors[i]});
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
    if (batch.size() >= batch_size && !flush_batch(provider, cache, batch, st, "memory")) return st;
  }
  (void)flush_batch(provider, cache, batch, st, "memory");  // st.ok set through the ref; discard intentional
  return st;
}

IndexStats index_sessions(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& sessions_dir, const std::string& exclude_path,
                          std::size_t batch_size) {
  namespace fs = std::filesystem;
  IndexStats st;
  if (batch_size == 0) batch_size = 1;
  std::error_code ec;
  if (!fs::exists(sessions_dir, ec)) return st;    // no past-session corpus yet -> empty stats
  // Canonicalize the live path ONCE; an empty exclude_path canonicalizes to a value no session
  // file matches, so nothing is excluded (the "no live session" case).
  const fs::path excl = fs::weakly_canonical(fs::path(exclude_path), ec);
  std::vector<std::pair<std::string, std::string>> batch;
  for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") continue;
    if (fs::weakly_canonical(entry.path(), ec) == excl) continue;     // skip the live session
    for (const auto& t : extract_session_turns(entry.path().string())) {
      if (cache.has(t.id)) { ++st.skipped; continue; }
      batch.emplace_back(t.id, t.text);
      if (batch.size() >= batch_size && !flush_batch(provider, cache, batch, st, "session")) return st;
    }
  }
  (void)flush_batch(provider, cache, batch, st, "session");
  return st;
}
}  // namespace hades
