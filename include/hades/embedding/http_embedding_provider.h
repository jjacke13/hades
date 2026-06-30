// include/hades/embedding/http_embedding_provider.h — OpenAI-compatible /embeddings provider.
//
// Posts {model, input:[texts]} to <endpoint>/embeddings over the injected HttpClient seam
// (same pattern as OpenAICompatProvider). Fail-soft: non-2xx status, malformed json, or a
// vector-count/dim mismatch sets EmbedResult.error (non-empty) and NEVER throws. Tests inject
// a fake HttpClient (no network).
#pragma once
#include <string>
#include "hades/embedding/provider.h"
#include "hades/llm/http.h"
namespace hades {
class HttpEmbeddingProvider : public EmbeddingProvider {
public:
  HttpEmbeddingProvider(std::string endpoint, std::string api_key, std::string model, HttpClient http);
  EmbedResult embed(const std::vector<std::string>& texts) override;
  std::string model() const override { return model_; }
private:
  std::string endpoint_, key_, model_;
  HttpClient http_;
};
}  // namespace hades
