// include/hades/module/embedding_memory_module.h — opt-in semantic-memory app (MOOS-app style).
//
// Mirrors MemoryModule but ranks by embeddings: on USER_MESSAGE it embeds the query (warm provider),
// cosine-ranks the VectorCache above min_similarity, then SPLITS the hits by src and posts two keys —
// RETRIEVED_MEMORY_SEMANTIC (archival fact hits) and RETRIEVED_SESSION_SEMANTIC (past-session excerpts).
// The Arbiter injects them as two labeled sub-blocks (facts vs past conversations). Corpus is indexed
// incrementally: on an Executor worker when set (live), else inline (tests). Every embedder failure is
// fail-soft — BOTH keys posted "" on any error path (empty result, never crashes a turn).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
  // Stops + joins the periodic reindex timer BEFORE the module's other members / the Blackboard are
  // destroyed — so no late timer tick run_index_()s (and bb_->post()s) into a dead bus. Load-bearing.
  ~EmbeddingMemoryModule();
  // Owns a thread/mutex/cv -> non-copyable, non-movable (only ever held by unique_ptr). Explicit for clarity.
  EmbeddingMemoryModule(const EmbeddingMemoryModule&) = delete;
  EmbeddingMemoryModule& operator=(const EmbeddingMemoryModule&) = delete;
  EmbeddingMemoryModule(EmbeddingMemoryModule&&) = delete;
  EmbeddingMemoryModule& operator=(EmbeddingMemoryModule&&) = delete;
  std::string type() const override { return "embedding_memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* ex) { executor_ = ex; }
  double reindex_interval_s() const { return reindex_interval_s_; }  // test seam (0 = off)
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
  double reindex_interval_s_ = kDefaultReindexIntervalS;  // re-run run_index_ every N s; 0 = launch-only
  // Serializes run_index_(): the initial Executor-worker index and a periodic timer tick (or two ticks
  // at a tiny interval) must not concurrently build+append the same cache file (-> duplicate lines).
  std::mutex index_mu_;
  Blackboard* bb_ = nullptr;
  Executor* executor_ = nullptr;
  // Periodic reindex timer (started in on_attach when reindex_interval_s_ > 0; stopped+joined in dtor).
  // The interruptible wait_for(predicate) lets the dtor wake the wait immediately (no full-interval
  // join stall). stop_reindex_ is set under reindex_mu_ so the predicate observes it under the same lock.
  std::thread reindex_thread_;
  std::atomic<bool> stop_reindex_{false};
  std::mutex reindex_mu_;
  std::condition_variable reindex_cv_;
};
}  // namespace hades
