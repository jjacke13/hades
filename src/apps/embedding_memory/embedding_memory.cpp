// src/apps/embedding_memory/embedding_memory.cpp — the semantic-memory app + its data layer
//
// Merged (2026-07-04 src reorg): module/embedding_memory_module (semantic rank per turn,
// incremental index, periodic reindex, fail-soft) + embedding/vector_cache (model-stamped
// flat store; the sqlite/ANN v2 seam) + embedding/indexer + embedding/session_turns +
// embedding/vec_math. Providers (HTTP + warm subprocess) live in providers.cpp next door.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include "hades/module/embedding_memory_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/embedding/http_embedding_provider.h"
#include "hades/embedding/indexer.h"
#include "hades/embedding/provider.h"
#include "hades/embedding/session_turns.h"
#include "hades/embedding/subprocess_embedding_provider.h"
#include "hades/embedding/vec_math.h"
#include "hades/embedding/vector_cache.h"
#include "hades/executor.h"
#include "hades/llm/http.h"
#include "hades/memory/store.h"

// ── EmbeddingMemoryModule: semantic rank per turn, incremental index, periodic reindex (was src/module/embedding_memory_module.cpp) ──────────────
namespace hades {
namespace {
std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v; std::istringstream is(s); std::string w; while (is >> w) v.push_back(w); return v;
}
std::string cache_path(const std::string& dir) { return dir + "/memory.vec.jsonl"; }
}  // namespace

EmbeddingMemoryModule::EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider> p) : provider_(std::move(p)) {}

EmbeddingMemoryModule::~EmbeddingMemoryModule() {
  // Stop + wake + join the timer FIRST (before any other member or the Blackboard is destroyed) so a
  // late tick can never run_index_()/bb_->post() into a torn-down module or a dead bus. notify_all
  // wakes the interruptible wait so join returns at once rather than after a full interval.
  { std::lock_guard<std::mutex> lk(reindex_mu_); stop_reindex_ = true; }
  reindex_cv_.notify_all();
  if (reindex_thread_.joinable()) reindex_thread_.join();
}

void EmbeddingMemoryModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("memory_store")) memory_store_ = cfg.kv.at("memory_store");
  if (cfg.kv.count("cache_dir")) cache_dir_ = cfg.kv.at("cache_dir");
  if (cfg.kv.count("sessions_dir")) sessions_dir_ = cfg.kv.at("sessions_dir");
  if (cfg.kv.count("index_sessions")) { bool b = index_sessions_; if (set_bool_on_string(cfg.kv.at("index_sessions"), b)) index_sessions_ = b; }
  if (cfg.kv.count("top_n")) { try { long n = std::stol(cfg.kv.at("top_n")); if (n > 0) top_n_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("batch_size")) { try { long n = std::stol(cfg.kv.at("batch_size")); if (n > 0) batch_size_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("min_similarity")) { double d = min_similarity_; if (set_pos_double_on_string(cfg.kv.at("min_similarity"), d)) min_similarity_ = static_cast<float>(d); }
  if (cfg.kv.count("timeout_s")) { double d = timeout_s_; if (set_pos_double_on_string(cfg.kv.at("timeout_s"), d)) timeout_s_ = d; }
  if (cfg.kv.count("reindex_interval_s")) {
    // Parse the value THEN branch (not via set_pos_double_on_string, which rejects 0): ANY zero
    // (0, 0.0) = off (launch-only); a positive value = the interval; a negative or garbage value
    // keeps the daily default (never silently disable a timer the operator intended to keep).
    double d;
    if (set_double_on_string(cfg.kv.at("reindex_interval_s"), d)) {
      if (d == 0.0) reindex_interval_s_ = 0.0;
      else if (d > 0.0) reindex_interval_s_ = d;
    }
  }
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
  // Serialize concurrent runs (initial Executor index vs a periodic timer tick) so they can't both
  // load the same cache, treat the same records as new, and double-append. Held only for the index;
  // the dtor stops the timer via reindex_mu_ (independent lock), so this never blocks teardown.
  std::lock_guard<std::mutex> index_lk(index_mu_);
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
  // Past-session corpus (per-turn), excluding the live mid-write session. Same cache + provider:
  // both corpora share the one model-stamped VectorCache (the query path ranks across both).
  if (index_sessions_) (void)index_sessions(*provider_, vc, sessions_dir_, live_session_path_, batch_size_);
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
    // Arbiter's objective veto() guard. Both keys are posted on EVERY path so neither is left stale.
    // Post both semantic keys on EVERY path (never leave a stale value from a prior turn).
    auto none = [this] {
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory");
      bb_->post("RETRIEVED_SESSION_SEMANTIC", "", "embedding_memory");
    };
    try {
      if (!e.value.is_string() || !provider_) { none(); return; }
      EmbedResult q = provider_->embed({e.value.get<std::string>()});
      if (!q.error.empty() || q.vectors.size() != 1) { none(); return; }
      VectorCache vc(cache_path(cache_dir_), q.model, q.dim);
      if (!vc.load()) { none(); return; }  // stamp mismatch -> keyword only
      auto top = vc.query(q.vectors[0], top_n_, min_similarity_);
      std::string facts, convos;
      for (const auto& r : top) {
        std::string& dst = (r.src == "session") ? convos : facts;  // memory/unknown src -> facts
        dst += "- " + r.text + "\n";
      }
      if (!facts.empty() && facts.back() == '\n') facts.pop_back();
      if (!convos.empty() && convos.back() == '\n') convos.pop_back();
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", facts, "embedding_memory");
      bb_->post("RETRIEVED_SESSION_SEMANTIC", convos, "embedding_memory");
    } catch (...) {
      none();  // fail-soft: never crash a turn
    }
  });
  if (executor_) executor_->submit([this] { run_index_(); });  // live: off the bus
  else run_index_();                                           // tests: inline + deterministic
  // Periodic reindex: re-run the (incremental) indexer every reindex_interval_s so a long-running
  // --serve picks up sessions that completed since launch. 0 = off (launch-only). run_index_ only
  // post()s (thread-safe) + appends the model-stamped cache file; the per-turn query re-opens the cache
  // read-only and tolerates a half-written final line, so no extra lock is needed against the query.
  // The subprocess provider serializes embed() with its own mutex (HTTP embed is per-call) -> the
  // timer's embeds and the pump-thread query's embeds don't race. Dtor stops+joins this thread.
  if (reindex_interval_s_ > 0) {
    reindex_thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lk(reindex_mu_);
      while (!stop_reindex_) {
        if (reindex_cv_.wait_for(lk, std::chrono::duration<double>(reindex_interval_s_),
                                 [this] { return stop_reindex_.load(); })) break;  // stopped during wait
        lk.unlock();
        // Guard the bare std::thread entry: an uncaught throw out of run_index_ would std::terminate
        // the whole process (unlike the Executor, which catches; and the query handler, which catches).
        // Unreachable today (both corpora are parse-validated UTF-8), but defense-in-depth matches the
        // feature's never-crash discipline.
        try { run_index_(); } catch (...) { /* skip this tick; the cache is unchanged or append-only */ }
        lk.lock();
      }
    });
  }
}
}  // namespace hades

// ── VectorCache: model-stamped flat store (the sqlite/ANN v2 seam) (was src/embedding/vector_cache.cpp) ──────────────
namespace hades {
VectorCache::VectorCache(std::string path, std::string model, int dim)
  : path_(std::move(path)), model_(std::move(model)), dim_(dim) {}

bool VectorCache::load() {
  mem_.clear();
  std::ifstream f(path_);
  if (!f) return true;                           // missing -> empty, not a mismatch
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    if (j.value("model", std::string{}) != model_ || j.value("dim", -1) != dim_) {
      mem_.clear(); return false;                // stamp mismatch -> caller rebuilds
    }
    if (!j.contains("vec") || !j["vec"].is_array()) continue;
    std::vector<float> v;
    bool ok = true;
    for (const auto& x : j["vec"]) { if (!x.is_number()) { ok = false; break; } v.push_back(x.get<float>()); }
    if (!ok || static_cast<int>(v.size()) != dim_) continue;
    mem_[j.value("id", std::string{})] = {j.value("text", std::string{}), j.value("src", std::string{}), std::move(v)};
  }
  return true;
}

bool VectorCache::has(const std::string& id) const { return mem_.count(id) > 0; }
std::vector<std::string> VectorCache::ids() const {
  std::vector<std::string> r; r.reserve(mem_.size());
  for (const auto& [id, _] : mem_) r.push_back(id);
  return r;
}

void VectorCache::put(const CachedVec& rec) {
  std::vector<float> v = rec.vec;
  if (!l2_normalize(v)) return;                  // degenerate -> drop
  if (dim_ <= 0) dim_ = static_cast<int>(v.size());
  if (static_cast<int>(v.size()) != dim_) return;
  mem_[rec.id] = {rec.text, rec.src, v};
  std::ofstream f(path_, std::ios::app);
  if (!f) return;                                // disk hiccup: in-memory still has it
  nlohmann::json j{{"id", rec.id}, {"src", rec.src}, {"model", model_}, {"dim", dim_}, {"text", rec.text}, {"vec", v}};
  // UTF-8-replace dump: rec.text is corpus text; a plain dump() throws on invalid UTF-8, which on the
  // index thread would escape run_index_. Replace instead (matches the subprocess-request dump).
  f << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
}

std::vector<ScoredMemory> VectorCache::query(std::vector<float> q, std::size_t top_n, float min_similarity) const {
  if (!l2_normalize(q)) return {};
  std::vector<ScoredMemory> scored;
  for (const auto& [id, r] : mem_) {
    float s = dot(q, r.vec);
    if (s >= min_similarity) scored.push_back({r.text, s, r.src});
  }
  std::sort(scored.begin(), scored.end(), [](const ScoredMemory& a, const ScoredMemory& b) { return a.score > b.score; });
  if (scored.size() > top_n) scored.resize(top_n);
  return scored;
}

void VectorCache::clear_file() {
  mem_.clear();
  std::ofstream f(path_, std::ios::trunc);       // truncate
}
}  // namespace hades

// ── indexer: incremental embed of archival + session corpora into the cache (was src/embedding/indexer.cpp) ──────────────
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

// ── vec_math: l2-normalize + dot product for cosine ranking (was src/embedding/vec_math.cpp) ──────────────
namespace hades {
bool l2_normalize(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  if (s <= 1e-12) return false;            // zero / degenerate
  const float inv = static_cast<float>(1.0 / std::sqrt(s));
  for (float& x : v) x *= inv;
  return true;
}
float dot(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 0.0f;
  float s = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}
}  // namespace hades
