// include/hades/embedding/vector_cache.h — model-stamped on-disk vector store + in-memory cosine query.
//
// One jsonl line per record {"id","src","model","dim","text","vec":[..]}. Vectors are L2-normalized
// at put() (a zero vector is dropped). load() returns FALSE if the file's stamped model/dim differ
// from the expected (model,dim) -> the caller rebuilds (clear_file + re-embed): comparing vectors
// from different models is garbage. Tolerant of blank/corrupt/partial lines.
#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>
namespace hades {
struct CachedVec { std::string id; std::string src; std::string text; std::vector<float> vec; };
struct ScoredMemory { std::string text; float score; };
class VectorCache {
public:
  VectorCache(std::string path, std::string model, int dim);
  bool load();                                   // false => model/dim mismatch (rebuild)
  bool has(const std::string& id) const;
  std::vector<std::string> ids() const;
  void put(const CachedVec& rec);                // normalizes; drops degenerate; appends a line
  std::vector<ScoredMemory> query(std::vector<float> q, std::size_t top_n, float min_similarity) const;
  std::size_t size() const { return mem_.size(); }
  void clear_file();                             // truncate the file + in-memory (rebuild path)
private:
  struct Rec { std::string text; std::string src; std::vector<float> vec; };
  std::string path_, model_;
  int dim_;
  std::map<std::string, Rec> mem_;               // id -> record (normalized vec)
};
}  // namespace hades
