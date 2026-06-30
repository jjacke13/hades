// include/hades/embedding/provider.h — text -> vectors. Two impls: warm subprocess / OpenAI-compat HTTP.
//
// embed() returns one vector per input (same order). On ANY failure it returns a result with a
// non-empty `error` (NOT a throw) — every caller fail-softs (index skips, query returns empty).
#pragma once
#include <string>
#include <vector>
namespace hades {
struct EmbedResult {
  std::vector<std::vector<float>> vectors;  // one per input text, in order
  std::string model;                        // model id that produced these (stamped into the cache)
  int dim = 0;
  std::string error;                        // non-empty => failure; caller MUST fail-soft
};
class EmbeddingProvider {
public:
  virtual ~EmbeddingProvider() = default;
  virtual EmbedResult embed(const std::vector<std::string>& texts) = 0;
  virtual std::string model() const = 0;    // the model id (for the cache stamp / mismatch check)
};
}  // namespace hades
