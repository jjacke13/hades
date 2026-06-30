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
#include "hades/timeouts.h"   // kDefaultLlmTimeoutS
namespace hades {
struct HttpResponse { long status; std::string body; };
using HttpClient = std::function<HttpResponse(
    const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& headers,
    const std::string& body)>;
// Per-call LLM HTTP cap. Manifest-configurable via Session `llm_timeout_s` (LLMModule
// passes the resolved value here). The default (kDefaultLlmTimeoutS=600s) is part of the
// turn-idle invariant: it must stay < turn_idle_timeout_s (default 900s), enforced in
// app/agent_wiring.cpp — see include/hades/timeouts.h.
HttpClient cpr_http(double timeout_s = kDefaultLlmTimeoutS);   // default impl uses cpr::Post
}  // namespace hades
