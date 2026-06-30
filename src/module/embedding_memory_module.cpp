#include "hades/module/embedding_memory_module.h"
#include <filesystem>
#include <sstream>
#include <system_error>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/embedding/http_embedding_provider.h"
#include "hades/embedding/indexer.h"
#include "hades/embedding/subprocess_embedding_provider.h"
#include "hades/embedding/vector_cache.h"
#include "hades/llm/http.h"
#include <cstdlib>
namespace hades {
namespace {
std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v; std::istringstream is(s); std::string w; while (is >> w) v.push_back(w); return v;
}
std::string cache_path(const std::string& dir) { return dir + "/memory.vec.jsonl"; }
}  // namespace

EmbeddingMemoryModule::EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider> p) : provider_(std::move(p)) {}

void EmbeddingMemoryModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("memory_store")) memory_store_ = cfg.kv.at("memory_store");
  if (cfg.kv.count("cache_dir")) cache_dir_ = cfg.kv.at("cache_dir");
  if (cfg.kv.count("top_n")) { try { long n = std::stol(cfg.kv.at("top_n")); if (n > 0) top_n_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("batch_size")) { try { long n = std::stol(cfg.kv.at("batch_size")); if (n > 0) batch_size_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("min_similarity")) { double d = min_similarity_; if (set_pos_double_on_string(cfg.kv.at("min_similarity"), d)) min_similarity_ = static_cast<float>(d); }
  if (cfg.kv.count("timeout_s")) { double d = timeout_s_; if (set_pos_double_on_string(cfg.kv.at("timeout_s"), d)) timeout_s_ = d; }
  if (!provider_) {                              // build from config (not the test-injected path)
    const std::string kind = cfg.kv.count("provider") ? cfg.kv.at("provider") : "subprocess";
    if (kind == "http") {
      const std::string ep = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
      const std::string model = cfg.kv.count("model") ? cfg.kv.at("model") : "";
      const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_EMBED_KEY";
      const char* key = std::getenv(env.c_str());
      provider_ = std::make_unique<HttpEmbeddingProvider>(ep, key ? key : "", model, cpr_http(timeout_s_));
    } else {                                     // subprocess (default)
      const std::string cmd = cfg.kv.count("command") ? cfg.kv.at("command") : "";
      provider_ = std::make_unique<SubprocessEmbeddingProvider>(split_ws(cmd), timeout_s_);
    }
  }
}

void EmbeddingMemoryModule::run_index_() {
  if (!provider_ || !bb_) return;
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  // Probe one embed to learn model+dim: a subprocess provider only knows them after the first reply,
  // and the cache must be stamped with the real (model,dim) so the per-turn query path can detect a
  // mismatch and rebuild rather than silently compare incomparable vectors.
  EmbedResult probe = provider_->embed({"_probe_"});
  if (!probe.error.empty()) { bb_->post("EMBED_INDEX_DONE", false, "embedding_memory"); return; }
  VectorCache vc(cache_path(cache_dir_), probe.model, probe.dim);
  if (!vc.load()) vc.clear_file();               // stamped model/dim changed -> rebuild from scratch
  index_archival(*provider_, vc, memory_store_, batch_size_);
  bb_->post("EMBED_INDEX_DONE", true, "embedding_memory");
}

void EmbeddingMemoryModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Subscribe BEFORE submitting the index worker — the Blackboard's subs list is not thread-safe
  // (see blackboard.h); the worker only post()s (thread-safe), but subscribing first keeps the
  // documented "all subscribe()s before any worker starts" convention with no reasoning needed.
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    // Whole handler in try/catch: this runs on the pump thread, so a throw here would unwind pump()
    // and kill the bus. The EmbeddingProvider contract is no-throw, but a misbehaving provider or
    // bad_alloc must still degrade to keyword-only (post "") — same fail-closed discipline as the
    // Arbiter's objective veto() guard. The key is posted on EVERY path so it is never left stale.
    try {
      if (!e.value.is_string() || !provider_) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }
      EmbedResult q = provider_->embed({e.value.get<std::string>()});
      if (!q.error.empty() || q.vectors.size() != 1) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }
      VectorCache vc(cache_path(cache_dir_), q.model, q.dim);
      if (!vc.load()) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }  // mismatch -> keyword only
      auto top = vc.query(q.vectors[0], top_n_, min_similarity_);
      std::string rendered;
      for (const auto& r : top) rendered += "- " + r.text + "\n";
      if (!rendered.empty() && rendered.back() == '\n') rendered.pop_back();
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", rendered, "embedding_memory");
    } catch (...) {
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory");  // fail-soft: never crash a turn
    }
  });
  if (executor_) executor_->submit([this] { run_index_(); });  // live: off the bus
  else run_index_();                                           // tests: inline + deterministic
}
}  // namespace hades
