// include/hades/module/embedding_memory_module.h — opt-in semantic-memory app (MOOS-app style).
//
// Mirrors MemoryModule but ranks by embeddings: on USER_MESSAGE it embeds the query (warm provider),
// cosine-ranks the VectorCache above min_similarity, and posts RETRIEVED_MEMORY_SEMANTIC (the Arbiter
// merges it with the keyword RETRIEVED_MEMORY). Corpus is indexed incrementally: on an Executor worker
// when set (live), else inline (tests). Every embedder failure is fail-soft (empty result, no crash).
#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "hades/embedding/defaults.h"
#include "hades/embedding/provider.h"
#include "hades/module.h"
namespace hades {
class Blackboard;
class Executor;
class EmbeddingMemoryModule : public Module {
public:
  EmbeddingMemoryModule() = default;
  explicit EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider> p);  // test injection
  std::string type() const override { return "embedding_memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* ex) { executor_ = ex; }
  // The live (current) session file to EXCLUDE from the past-session corpus pass — it is mid-write.
  // Wired (in wire_agent) to the SAME path the Arbiter persists to, and MUST be called BEFORE
  // on_attach: on_attach is what submits the index worker (which reads live_session_path_), so the
  // setter's write must happen-before that submit. The Executor queue's synchronization then makes
  // the write visible to the worker race-free (no data race, and a resumed session is correctly
  // excluded rather than embedded mid-write). Calling it after on_attach would re-introduce the race.
  void set_live_session_path(std::string p) { live_session_path_ = std::move(p); }
private:
  void run_index_();                            // incremental index of the archival + session corpora
  std::unique_ptr<EmbeddingProvider> provider_;
  std::string memory_store_ = ".hades/memory.jsonl";
  std::string cache_dir_ = ".hades/embeddings";
  std::string sessions_dir_ = ".hades/sessions";
  bool index_sessions_ = true;
  std::string live_session_path_;
  std::size_t top_n_ = kDefaultEmbedTopN;
  float min_similarity_ = kDefaultMinSimilarity;
  std::size_t batch_size_ = kDefaultEmbedBatch;
  double timeout_s_ = kDefaultEmbedTimeoutS;
  Blackboard* bb_ = nullptr;
  Executor* executor_ = nullptr;
};
}  // namespace hades
