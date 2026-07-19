// include/hades/search/search_config.h — web_search provider config: presets + argv k=v codec
//
// One generic HTTP search engine lives in tools/web_search_main.cpp; this header is the
// shared config contract: the SearchConfig knobs, the named presets (searxng/brave/http)
// that pre-fill them, the manifest-Block resolver wiring uses (fail-loud MalConfig), and
// the argv k=v codec that carries the RESOLVED config into the tool subprocess — the
// single-source-of-truth pattern (the LLM can never redirect the endpoint). The API key
// itself never enters argv (world-readable via /proc): only the env var NAME travels and
// the tool getenv()s it. Header-only so the standalone tool binary shares the exact codec
// without linking hades_core.
#pragma once
#include <cctype>
#include <string>
#include "hades/config.h"     // Block
#include "hades/launcher.h"   // MalConfig
namespace hades {

struct SearchConfig {
  std::string provider;                     // searxng | brave | http (validated)
  std::string endpoint;                     // searxng: base (+"/search"); brave/http: full URL
  std::string api_key_env;                  // env var NAME holding the key; "" = no auth
  std::string method = "get";               // get | post (post body = {query_param: query})
  std::string query_param = "q";
  std::string auth_header = "Authorization";
  std::string auth_scheme = "bearer";       // bearer ("Bearer <key>") | none (raw key)
  std::string results_path = "/results";    // nlohmann JSON pointer to the results array
  std::string title_key = "title";
  std::string url_key = "url";
  std::string snippet_key = "snippet";
  std::string extra_query;                  // one literal k=v appended to a GET query
  int max_results = 5;                      // clamp 1..10
  int timeout_s = 15;
};

// Resolve a `Search { … }` manifest block: preset fills knobs, explicit keys override,
// structural problems throw MalConfig (fail-loud at launch). PURE — the api_key_env
// PRESENCE gate (getenv) is the wiring's job, kept out of here for testability.
inline SearchConfig resolve_search_config(const Block& search_cfg) {
  auto get = [&](const char* k) -> std::string {
    auto it = search_cfg.kv.find(k);
    return it != search_cfg.kv.end() ? it->second : std::string{};
  };
  SearchConfig c;
  c.provider = get("provider");
  if (c.provider == "searxng") {
    c.snippet_key = "content";
    c.extra_query = "format=json";
  } else if (c.provider == "brave") {
    c.endpoint = "https://api.search.brave.com/res/v1/web/search";
    c.auth_header = "X-Subscription-Token";
    c.auth_scheme = "none";
    c.results_path = "/web/results";
    c.snippet_key = "description";
    c.api_key_env = "HADES_SEARCH_KEY";
  } else if (c.provider == "http") {
    // raw knobs — table defaults stand
  } else {
    throw MalConfig("Search block: provider must be searxng | brave | http");
  }
  // explicit manifest keys beat preset values
  auto ovr = [&](const char* k, std::string& field) {
    auto it = search_cfg.kv.find(k);
    if (it != search_cfg.kv.end()) field = it->second;
  };
  ovr("endpoint", c.endpoint);
  ovr("api_key_env", c.api_key_env);
  ovr("method", c.method);
  ovr("query_param", c.query_param);
  ovr("auth_header", c.auth_header);
  ovr("auth_scheme", c.auth_scheme);
  ovr("results_path", c.results_path);
  ovr("title_key", c.title_key);
  ovr("url_key", c.url_key);
  ovr("snippet_key", c.snippet_key);
  ovr("extra_query", c.extra_query);
  auto num = [&](const char* k, int def) {
    auto it = search_cfg.kv.find(k);
    if (it == search_cfg.kv.end()) return def;
    try {
      int v = std::stoi(it->second);
      return v > 0 ? v : def;              // garbage/non-positive -> default, never 0
    } catch (...) {
      return def;
    }
  };
  c.max_results = num("max_results", 5);
  if (c.max_results > 10) c.max_results = 10;   // snippet token budget
  c.timeout_s = num("timeout_s", 15);
  if (c.endpoint.empty()) throw MalConfig("Search block requires endpoint");
  if (c.method != "get" && c.method != "post")
    throw MalConfig("Search block: method must be get | post");
  if (c.auth_scheme != "bearer" && c.auth_scheme != "none")
    throw MalConfig("Search block: auth_scheme must be bearer | none");
  return c;
}

// Serialize the resolved config as " k=v k=v …" for the tool argv. Values must be
// whitespace-free (argv is whitespace-split) — a violating value throws MalConfig
// naming the key. Empty-valued keys are omitted (parse side keeps its default).
inline std::string to_argv_kv(const SearchConfig& c) {
  std::string out;
  auto put = [&](const char* k, const std::string& v) {
    if (v.empty()) return;
    for (unsigned char ch : v)
      if (std::isspace(ch))
        throw MalConfig(std::string("Search config value for '") + k +
                        "' must not contain whitespace");
    out += ' ';
    out += k;
    out += '=';
    out += v;
  };
  put("provider", c.provider);
  put("endpoint", c.endpoint);
  put("api_key_env", c.api_key_env);
  put("method", c.method);
  put("query_param", c.query_param);
  put("auth_header", c.auth_header);
  put("auth_scheme", c.auth_scheme);
  put("results_path", c.results_path);
  put("title_key", c.title_key);
  put("url_key", c.url_key);
  put("snippet_key", c.snippet_key);
  put("extra_query", c.extra_query);
  put("max_results", std::to_string(c.max_results));
  put("timeout_s", std::to_string(c.timeout_s));
  return out;
}

// Tool-side parse: fold k=v args over the defaults. Splits on the FIRST '=' only (an
// extra_query value legitimately contains '='). Unknown keys and non-k=v tokens are
// ignored (forward-compat; a subprocess must never die on its own argv). Never throws.
inline SearchConfig parse_argv_kv(int argc, char** argv) {
  SearchConfig c;
  for (int i = 1; i < argc; ++i) {
    const std::string tok = argv[i] ? argv[i] : "";
    const std::size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string k = tok.substr(0, eq);
    const std::string v = tok.substr(eq + 1);
    if (k == "provider") c.provider = v;
    else if (k == "endpoint") c.endpoint = v;
    else if (k == "api_key_env") c.api_key_env = v;
    else if (k == "method") c.method = v;
    else if (k == "query_param") c.query_param = v;
    else if (k == "auth_header") c.auth_header = v;
    else if (k == "auth_scheme") c.auth_scheme = v;
    else if (k == "results_path") c.results_path = v;
    else if (k == "title_key") c.title_key = v;
    else if (k == "url_key") c.url_key = v;
    else if (k == "snippet_key") c.snippet_key = v;
    else if (k == "extra_query") c.extra_query = v;
    else if (k == "max_results") { try { int n = std::stoi(v); if (n > 0) c.max_results = n; } catch (...) {} }
    else if (k == "timeout_s") { try { int n = std::stoi(v); if (n > 0) c.timeout_s = n; } catch (...) {} }
  }
  return c;
}

}  // namespace hades
