// src/embedding/http_embedding_provider.cpp — OpenAI-compatible /embeddings impl.
//
// embed() serialises {model, input:[texts]} to a JSON body, dispatches it via the injected
// HttpClient, and parses the {"data":[{"embedding":[...]}]} response into EmbedResult.vectors
// (one per input, in order). Every external value is type/bounds-checked before use; ANY failure
// (http throw, non-2xx, unparseable/wrong-shape json, non-number value, count/dim mismatch) sets
// out.error and returns — NEVER throws.
#include "hades/embedding/http_embedding_provider.h"
#include <nlohmann/json.hpp>
namespace hades {
HttpEmbeddingProvider::HttpEmbeddingProvider(std::string e, std::string k, std::string m, HttpClient h)
  : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), http_(std::move(h)) {}

EmbedResult HttpEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  EmbedResult out;
  out.model = model_;
  nlohmann::json body{{"model", model_}, {"input", texts}};
  HttpResponse resp;
  try {
    resp = http_(endpoint_ + "/embeddings",
                 {{"Authorization", "Bearer " + key_}, {"Content-Type", "application/json"}},
                 body.dump());
  } catch (...) { out.error = "embedding http call threw"; return out; }
  if (resp.status < 200 || resp.status >= 300) { out.error = "embedding http status " + std::to_string(resp.status); return out; }
  auto j = nlohmann::json::parse(resp.body, nullptr, false);
  if (j.is_discarded() || !j.contains("data") || !j["data"].is_array()) { out.error = "embedding response not parseable"; return out; }
  for (const auto& d : j["data"]) {
    if (!d.is_object() || !d.contains("embedding") || !d["embedding"].is_array()) { return EmbedResult{{}, model_, 0, "embedding item malformed"}; }
    std::vector<float> v;
    for (const auto& x : d["embedding"]) { if (!x.is_number()) { return EmbedResult{{}, model_, 0, "embedding value not number"}; } v.push_back(x.get<float>()); }
    out.vectors.push_back(std::move(v));
  }
  if (out.vectors.size() != texts.size()) { return EmbedResult{{}, model_, 0, "embedding count mismatch"}; }
  out.dim = out.vectors.empty() ? 0 : static_cast<int>(out.vectors.front().size());
  for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, model_, 0, "embedding dim inconsistent"}; }
  return out;
}
}  // namespace hades
