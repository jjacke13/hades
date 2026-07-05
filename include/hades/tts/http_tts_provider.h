// include/hades/tts/http_tts_provider.h — OpenAI-compatible /audio/speech provider
//
// POSTs {model, input, voice, response_format:"opus"} to <endpoint>/audio/speech and returns the raw
// response body (ogg-opus bytes). REUSES the existing HttpClient/cpr_http seam (llm/http.h) — unlike
// STT, /audio/speech is a plain JSON POST returning bytes, so no custom multipart client is needed
// (HttpResponse.body is byte-safe). Fail-soft: non-2xx, empty body, or a transport throw -> {ok:false}.
// Endpoint is the BASE url; this appends /audio/speech (same gotcha as embedding's /embeddings).
#pragma once
#include <string>
#include "hades/tts/provider.h"
#include "hades/llm/http.h"   // HttpClient, HttpResponse, cpr_http
namespace hades {
class HttpTtsProvider : public TtsProvider {
 public:
  HttpTtsProvider(std::string endpoint, std::string api_key, std::string model, std::string voice,
                  HttpClient http);
  TtsResult synthesize(const std::string& text) override;

 private:
  std::string endpoint_, key_, model_, voice_;
  HttpClient http_;
};
}  // namespace hades
