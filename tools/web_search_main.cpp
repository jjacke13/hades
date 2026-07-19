// tools/web_search_main.cpp — bundled web_search native tool binary
//
// Reads one JSON line ({"call":"describe"|"web_search","args":{query,max_results?}}),
// queries the operator-configured search backend, and writes one JSON line
// {ok, result:{results:[{title,url,snippet}],provider}}. The ENTIRE provider config
// arrives as argv k=v pairs pinned by the wiring (presets resolved there) — the LLM
// supplies only the query text, so there is no SSRF surface in the arguments and the
// endpoint (often a loopback/LAN SearXNG instance) is operator-trusted; for that same
// reason cpr's default redirect-following stays ON here, unlike http_fetch whose URLs
// are LLM-chosen. The API key never appears in argv (/proc-visible): argv carries the
// env var NAME and the key is read from the environment. Fail-soft: every backend
// problem returns ok:false, never a crash; reply uses the UTF-8-replace dump (snippet
// byte-caps can split codepoints).
#include <cstdlib>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hades/search/search_config.h"

int main(int argc, char** argv) {
  const hades::SearchConfig cfg = hades::parse_argv_kv(argc, argv);
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "web_search"},
             {"description",
              "Search the web and return ranked results (title, url, snippet). Follow up "
              "promising results with http_fetch to read the page."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"query", {{"type", "string"}}},
                 {"max_results",
                  {{"type", "number"},
                   {"description", "how many results to return (1-10, default 5)"}}}}},
               {"required", {"query"}}}}}}};
  } else if (call == "web_search") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    const bool has_query = args.contains("query") && args["query"].is_string();
    const std::string query = has_query ? args["query"].get<std::string>() : "";
    int max_results = cfg.max_results;
    if (args.contains("max_results") && args["max_results"].is_number()) {
      const int n = args["max_results"].get<int>();
      if (n > 0) max_results = n;          // non-positive = absent (house rule)
    }
    if (max_results > 10) max_results = 10;

    std::string key;
    bool key_missing = false;
    if (!cfg.api_key_env.empty()) {
      const char* k = std::getenv(cfg.api_key_env.c_str());
      if (k && *k) key = k;
      else key_missing = true;             // boot checks too; this is defense in depth
    }

    if (query.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: query"}}}};
    } else if (key_missing) {
      out = {{"ok", false},
             {"result", {{"error", "search api key env " + cfg.api_key_env + " is not set"}}}};
    } else {
      cpr::Header header;
      if (!key.empty())
        header[cfg.auth_header] = cfg.auth_scheme == "bearer" ? "Bearer " + key : key;
      header["Accept"] = "application/json";
      const std::string url =
          cfg.endpoint + (cfg.provider == "searxng" ? "/search" : "");
      cpr::Response r;
      if (cfg.method == "post") {
        nlohmann::json body{{cfg.query_param, query}};
        header["Content-Type"] = "application/json";
        r = cpr::Post(cpr::Url{url}, cpr::Body{body.dump()}, header,
                      cpr::Timeout{cfg.timeout_s * 1000});
      } else {
        cpr::Parameters params{{cfg.query_param, query}};
        if (!cfg.extra_query.empty()) {
          const std::size_t eq = cfg.extra_query.find('=');
          if (eq != std::string::npos && eq > 0)
            params.Add({cfg.extra_query.substr(0, eq), cfg.extra_query.substr(eq + 1)});
        }
        r = cpr::Get(cpr::Url{url}, params, header, cpr::Timeout{cfg.timeout_s * 1000});
      }

      if (r.status_code != 200) {
        std::string err = "search backend returned status " +
                          std::to_string(static_cast<int>(r.status_code));
        if (r.status_code == 403 && cfg.provider == "searxng")
          err += " (SearXNG: enable the json format in settings.yml search.formats)";
        out = {{"ok", false},
               {"result", {{"error", err}, {"status", static_cast<int>(r.status_code)}}}};
      } else {
        auto j = nlohmann::json::parse(r.text, nullptr, false);
        nlohmann::json hits = nlohmann::json::array();
        bool path_ok = false;
        if (!j.is_discarded()) {
          try {
            const nlohmann::json::json_pointer ptr(cfg.results_path);
            if (j.contains(ptr) && j.at(ptr).is_array()) {
              path_ok = true;
              // Type-safe field read: real backends (SearXNG engines, Brave) routinely emit
              // null descriptions — e.value() THROWS on a null/non-string field, and one bad
              // element must degrade per-entry, never abort the whole result walk.
              auto sv = [](const nlohmann::json& o, const std::string& k) {
                auto it = o.find(k);
                return it != o.end() && it->is_string() ? it->get<std::string>()
                                                        : std::string{};
              };
              for (const auto& e : j.at(ptr)) {
                if (static_cast<int>(hits.size()) >= max_results) break;
                if (!e.is_object()) continue;
                const std::string title = sv(e, cfg.title_key);
                const std::string href = sv(e, cfg.url_key);
                if (title.empty() || href.empty()) continue;   // spec: skip incomplete
                std::string snippet = sv(e, cfg.snippet_key);
                constexpr std::size_t kSnippetCap = 500;
                if (snippet.size() > kSnippetCap) snippet.resize(kSnippetCap);
                hits.push_back({{"title", title}, {"url", href}, {"snippet", snippet}});
              }
            }
          } catch (...) {
            path_ok = false;               // invalid pointer syntax etc. — fail soft
          }
        }
        if (!path_ok) {
          out = {{"ok", false},
                 {"result",
                  {{"error", "no results array at " + cfg.results_path +
                                 " in the backend response"}}}};
        } else {
          out = {{"ok", true},
                 {"result", {{"results", hits}, {"provider", cfg.provider}}}};
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
            << std::endl;
  return 0;
}
