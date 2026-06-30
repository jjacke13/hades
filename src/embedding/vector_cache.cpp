#include "hades/embedding/vector_cache.h"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include "hades/embedding/vec_math.h"
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
    if (s >= min_similarity) scored.push_back({r.text, s});
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
