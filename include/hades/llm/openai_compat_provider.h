// include/hades/llm/openai_compat_provider.h — OpenAI-compatible Provider
//
// Concrete Provider that serialises LlmRequest to the OpenAI chat-completions
// JSON format and POSTs via an injected HttpClient. Used by LLMModule;
// build_body() is exposed so tests can verify the request payload directly.

#pragma once
#include "hades/llm/provider.h"
#include "hades/llm/http.h"
namespace hades {
class OpenAICompatProvider : public Provider {
public:
  OpenAICompatProvider(std::string endpoint, std::string api_key, std::string model, HttpClient http);
  LlmResponse complete(const LlmRequest&) override;
  nlohmann::json build_body(const LlmRequest&) const;   // exposed for tests
private:
  std::string endpoint_, key_, model_; HttpClient http_;
};
}  // namespace hades
