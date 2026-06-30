#include "hades/embedding/subprocess_embedding_provider.h"
#include <nlohmann/json.hpp>
namespace hades {
SubprocessEmbeddingProvider::SubprocessEmbeddingProvider(std::vector<std::string> argv, double timeout_s)
  : child_(std::move(argv), timeout_s) {}

std::string SubprocessEmbeddingProvider::model() const {
  std::lock_guard<std::mutex> lk(mu_);          // model_ is written under mu_ by embed() — avoid a torn read
  return model_;
}

EmbedResult SubprocessEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  std::lock_guard<std::mutex> lk(mu_);          // serialize the shared warm child
  EmbedResult out;
  nlohmann::json req{{"texts", texts}};
  // UTF-8-replace dump: query/corpus text is user/LLM-sourced and may contain invalid UTF-8;
  // a plain dump() would THROW (uncaught -> the fail-soft contract broken). Replace instead.
  auto rep = child_.request(req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  if (!rep.ok) { out.error = rep.err; return out; }
  // Wrap the WHOLE parse->validate->extract in try/catch: nlohmann's value()/get<>() throw a
  // type_error (302) on a present-but-wrong-typed field (e.g. {"error":{...}}, {"model":5},
  // {"dim":"x"}) — parse(...,false) does NOT catch that (valid JSON, throw at extraction).
  // Any such throw becomes a soft error so embed() upholds its "never throws" contract.
  try {
    auto j = nlohmann::json::parse(rep.line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { out.error = "embedder reply not json object"; return out; }
    if (j.contains("error")) { out.error = "embedder error: " + j.value("error", std::string{"?"}); return out; }
    if (!j.contains("embeddings") || !j["embeddings"].is_array()) { out.error = "embedder reply missing embeddings"; return out; }
    out.model = j.value("model", std::string{});
    out.dim = j.value("dim", 0);
    for (const auto& row : j["embeddings"]) {
      if (!row.is_array()) { return EmbedResult{{}, out.model, 0, "embedder row not array"}; }
      std::vector<float> v;
      for (const auto& x : row) { if (!x.is_number()) { return EmbedResult{{}, out.model, 0, "embedder value not number"}; } v.push_back(x.get<float>()); }
      out.vectors.push_back(std::move(v));
    }
    if (out.vectors.size() != texts.size()) { return EmbedResult{{}, out.model, 0, "embedder count mismatch"}; }
    if (out.dim <= 0 && !out.vectors.empty()) out.dim = static_cast<int>(out.vectors.front().size());
    for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, out.model, 0, "embedder dim inconsistent"}; }
    if (!out.model.empty()) model_ = out.model;
    return out;
  } catch (const std::exception& e) {
    return EmbedResult{{}, "", 0, std::string("embedder reply parse error: ") + e.what()};
  }
}
}  // namespace hades
