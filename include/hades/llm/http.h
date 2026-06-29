// include/hades/llm/http.h — injected HTTP transport for LLM calls
//
// Defines HttpResponse and HttpClient (a std::function type alias) — the
// transport interface injected into OpenAICompatProvider. cpr_http() is the
// default cpr-backed implementation; tests may substitute a stub.

#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace hades {
struct HttpResponse { long status; std::string body; };
using HttpClient = std::function<HttpResponse(
    const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& headers,
    const std::string& body)>;
// The 120s default cap is part of the kTurnTimeoutS / kCollectTimeoutS idle-timeout
// invariant (must stay < that ~180s idle window) — see src/module/chat_module.cpp.
HttpClient cpr_http(double timeout_s = 120.0);   // default impl uses cpr::Post
}  // namespace hades
