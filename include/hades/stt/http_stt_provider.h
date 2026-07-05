// include/hades/stt/http_stt_provider.h — OpenAI-compatible /audio/transcriptions provider
//
// POSTs a multipart form (file + model + optional language) to <endpoint>/audio/transcriptions over
// an injected SttHttpClient seam (the embedding provider's injected-HttpClient pattern — tests feed
// a fake with no socket; the glibc-getaddrinfo TSan caveat is why it is injected). Parses {"text":…}.
// Fail-soft: non-2xx, unparseable json, missing text, or a transport throw -> {ok:false}. Endpoint is
// the BASE url; this appends /audio/transcriptions (same gotcha as embedding's /embeddings).
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "hades/stt/provider.h"
#include "hades/llm/http.h"   // HttpResponse
namespace hades {
using SttHttpClient = std::function<HttpResponse(
    const std::string& url, const std::string& bearer, const std::string& audio_path,
    const std::vector<std::pair<std::string, std::string>>& fields)>;
SttHttpClient cpr_stt_http(double timeout_s);   // default cpr multipart impl (redirects off)

class HttpSttProvider : public SttProvider {
 public:
  HttpSttProvider(std::string endpoint, std::string api_key, std::string model,
                  std::string language, SttHttpClient http);
  SttResult transcribe(const std::string& audio_path) override;

 private:
  std::string endpoint_, key_, model_, language_;
  SttHttpClient http_;
};
}  // namespace hades
