# web_search Native Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `web_search {query, max_results?}` native tool returning `{title, url, snippet}` results from an operator-configured backend (SearXNG self-hosted default, Brave preset, raw `http` knobs for other hosted APIs) — the discovery half that composes with the just-shipped `http_fetch` extraction.

**Architecture:** One generic HTTP search engine in a single tool binary; named presets fill its knobs. A header-only `SearchConfig` (struct + preset table + `Block` resolver + argv `k=v` codec) is shared by wiring, tool, and tests without a core link. Wiring resolves the `Search` manifest block and pins the WHOLE config into the tool's argv (`k=v` pairs) — the LLM can never redirect the endpoint; the API key travels as an env-var NAME only (never in argv — /proc-visible). New `Capability::WebSearch` → allow.

**Tech Stack:** C++20, nlohmann_json (incl. `json_pointer`), cpr (tool), httplib (tests only), GoogleTest, CMake+Ninja in `nix develop`.

**Spec:** `docs/superpowers/specs/2026-07-18-web-search-design.md` (approved, committed `c11bfc4`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **715/715 green** (both lanes) before Task 1. `build/` carries `-fsanitize=address,undefined` (restored 2026-07-18 — do NOT reconfigure it away; full sanitized suite ~110s).
- Branch `feat/web-search` (created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- Tool contract: `web_search {query, max_results?}` → `{ok, result:{results:[{title,url,snippet}], provider}}`. Empty/non-string query fails closed; non-number/non-positive `max_results` = absent; default **5**, clamp **1..10**; zero hits → `ok:true` + empty array; snippet byte-cap **500**; reply via the UTF-8-replace dump `out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)`.
- Describe text exactly: `Search the web and return ranked results (title, url, snippet). Follow up promising results with http_fetch to read the page.`
- Presets exactly as the spec table: `searxng` (path `/search` appended to endpoint, `extra_query = format=json`, `results_path = /results`, `snippet_key = content`, no auth, endpoint REQUIRED) · `brave` (`endpoint = https://api.search.brave.com/res/v1/web/search` used as full URL, `auth_header = X-Subscription-Token`, `auth_scheme = none`, `results_path = /web/results`, `snippet_key = description`, `api_key_env = HADES_SEARCH_KEY`) · `http` (raw knobs, endpoint REQUIRED). Explicit manifest key beats preset value.
- MalConfig gates (fail loud at boot): `web_search` rostered with no `Search` block · resolved config missing `endpoint` · `api_key_env` non-empty but env unset/empty · any argv-bound config value containing whitespace · unknown `provider`/`method`/`auth_scheme`.
- API key NEVER in argv; tool `getenv`s the name. `HADES_SEARCH_KEY` (or the configured `api_key_env`) value is added to the Eventlog redaction list in `hades_main`.
- The tool follows cpr's default redirect behavior (endpoint is operator-trusted — unlike http_fetch's LLM-chosen URLs; say so in a comment).
- File headers house style: `// path — one-line purpose` + short explanation block.

---

## File Structure

```
include/hades/search/search_config.h  T1  header-only: SearchConfig, presets, resolver, argv k=v codec
tests/test_search_config.cpp          T1
tools/web_search_main.cpp             T2  the engine binary (cpr GET/POST, auth, pointer walk, mapping)
tests/test_web_search_tool.cpp        T2  loopback httplib fakes (searxng shape, brave shape, POST shape)
package.nix                           T2  add "hades-web-search" to the bins list (Pi static build — session-search lesson)
include/hades/objective/capability_policy.h  T3  WebSearch enum
src/behaviors/capability_policy.cpp   T3  capability row + allow case
app/agent_wiring.cpp + .h             T3  Search block resolve + argv append + env gate
app/hades_main.cpp                    T3  redaction
tests/test_web_search_wiring.cpp      T3
tests/test_capability_policy.cpp      T3  (append rows)
CMakeLists.txt                        T1,T2,T3
docs/manifest-reference.md, manifests/dev.hades, .env.example, CLAUDE.md, README.md  T4
```

---

## Task 1: `SearchConfig` header — presets, resolver, argv codec

**Files:**
- Create: `include/hades/search/search_config.h`
- Test: `tests/test_search_config.cpp`
- Modify: `CMakeLists.txt` (one test-source line)

**Interfaces — Produces (all `namespace hades`, header-only `inline`):**
- `struct SearchConfig { std::string provider, endpoint, api_key_env, method, query_param, auth_header, auth_scheme, results_path, title_key, url_key, snippet_key, extra_query; int max_results; int timeout_s; }` (defaults per the table below).
- `SearchConfig resolve_search_config(const Block& search_cfg)` — preset + overrides; throws `MalConfig` on structural problems (PURE — no getenv; the env-presence gate is wiring's, Task 3).
- `std::string to_argv_kv(const SearchConfig&)` — `" k=v k=v …"` (leading space, appendable to a native cmd); throws `MalConfig` if any value contains whitespace.
- `SearchConfig parse_argv_kv(int argc, char** argv)` — tool side; starts from defaults, folds `k=v` args, ignores unknown keys and non-`k=v` tokens; never throws.

- [ ] **Step 1: Write the failing tests** `tests/test_search_config.cpp`:

```cpp
// tests/test_search_config.cpp — SearchConfig presets, overrides, MalConfig gates, argv codec
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "hades/launcher.h"
#include "hades/search/search_config.h"
using namespace hades;

static Block mk(const std::string& provider,
                std::initializer_list<std::pair<std::string, std::string>> kv = {}) {
  Block b;
  b.section = "Search";
  if (!provider.empty()) b.kv["provider"] = provider;
  for (const auto& [k, v] : kv) b.kv[k] = v;
  return b;
}

TEST(SearchConfig, SearxngPresetFillsKnobs) {
  auto c = resolve_search_config(mk("searxng", {{"endpoint", "http://127.0.0.1:8888"}}));
  EXPECT_EQ(c.provider, "searxng");
  EXPECT_EQ(c.endpoint, "http://127.0.0.1:8888");
  EXPECT_EQ(c.extra_query, "format=json");
  EXPECT_EQ(c.results_path, "/results");
  EXPECT_EQ(c.snippet_key, "content");
  EXPECT_EQ(c.api_key_env, "");            // no auth
  EXPECT_EQ(c.method, "get");
  EXPECT_EQ(c.max_results, 5);
  EXPECT_EQ(c.timeout_s, 15);
}

TEST(SearchConfig, BravePresetDefaultsEndpointAndAuth) {
  auto c = resolve_search_config(mk("brave"));
  EXPECT_EQ(c.endpoint, "https://api.search.brave.com/res/v1/web/search");
  EXPECT_EQ(c.auth_header, "X-Subscription-Token");
  EXPECT_EQ(c.auth_scheme, "none");
  EXPECT_EQ(c.results_path, "/web/results");
  EXPECT_EQ(c.snippet_key, "description");
  EXPECT_EQ(c.api_key_env, "HADES_SEARCH_KEY");
}

TEST(SearchConfig, ExplicitKeyBeatsPreset) {
  auto c = resolve_search_config(
      mk("brave", {{"snippet_key", "summary"}, {"api_key_env", "MY_KEY"}}));
  EXPECT_EQ(c.snippet_key, "summary");
  EXPECT_EQ(c.api_key_env, "MY_KEY");
  EXPECT_EQ(c.auth_header, "X-Subscription-Token");   // untouched preset value stays
}

TEST(SearchConfig, HttpProviderIsRawDefaults) {
  auto c = resolve_search_config(mk("http", {{"endpoint", "https://api.example/v1"}}));
  EXPECT_EQ(c.results_path, "/results");
  EXPECT_EQ(c.snippet_key, "snippet");
  EXPECT_EQ(c.auth_header, "Authorization");
  EXPECT_EQ(c.auth_scheme, "bearer");
  EXPECT_EQ(c.api_key_env, "");
}

TEST(SearchConfig, MalConfigGates) {
  EXPECT_THROW(resolve_search_config(mk("")), MalConfig);              // provider required
  EXPECT_THROW(resolve_search_config(mk("google")), MalConfig);        // unknown provider
  EXPECT_THROW(resolve_search_config(mk("searxng")), MalConfig);       // endpoint required
  EXPECT_THROW(resolve_search_config(mk("http")), MalConfig);          // endpoint required
  EXPECT_THROW(resolve_search_config(
                   mk("searxng", {{"endpoint", "http://x"}, {"method", "put"}})),
               MalConfig);                                             // bad method
  EXPECT_THROW(resolve_search_config(
                   mk("searxng", {{"endpoint", "http://x"}, {"auth_scheme", "digest"}})),
               MalConfig);                                             // bad auth_scheme
}

TEST(SearchConfig, NumbersTolerantAndBounded) {
  auto c = resolve_search_config(mk("searxng", {{"endpoint", "http://x"},
                                                {"max_results", "garbage"},
                                                {"timeout_s", "-3"}}));
  EXPECT_EQ(c.max_results, 5);    // garbage -> default
  EXPECT_EQ(c.timeout_s, 15);     // non-positive -> default
  auto d = resolve_search_config(mk("searxng", {{"endpoint", "http://x"},
                                                {"max_results", "50"}}));
  EXPECT_EQ(d.max_results, 10);   // clamp 1..10
}

TEST(SearchConfig, ArgvRoundTrip) {
  auto c = resolve_search_config(
      mk("brave", {{"api_key_env", "MY_KEY"}, {"max_results", "7"}}));
  const std::string argv_tail = to_argv_kv(c);
  EXPECT_EQ(argv_tail.rfind(' ', 0), 0u);   // leading space, appendable
  // split on spaces -> argv vector, prepend a fake binary name
  std::vector<std::string> parts{"bin"};
  std::size_t pos = 1;
  while (pos < argv_tail.size()) {
    std::size_t sp = argv_tail.find(' ', pos);
    if (sp == std::string::npos) sp = argv_tail.size();
    if (sp > pos) parts.push_back(argv_tail.substr(pos, sp - pos));
    pos = sp + 1;
  }
  std::vector<char*> argv;
  for (auto& p : parts) argv.push_back(p.data());
  auto back = parse_argv_kv(static_cast<int>(argv.size()), argv.data());
  EXPECT_EQ(back.provider, c.provider);
  EXPECT_EQ(back.endpoint, c.endpoint);
  EXPECT_EQ(back.api_key_env, "MY_KEY");
  EXPECT_EQ(back.auth_header, c.auth_header);
  EXPECT_EQ(back.auth_scheme, c.auth_scheme);
  EXPECT_EQ(back.results_path, c.results_path);
  EXPECT_EQ(back.snippet_key, c.snippet_key);
  EXPECT_EQ(back.max_results, 7);
  EXPECT_EQ(back.timeout_s, c.timeout_s);
}

TEST(SearchConfig, ExtraQueryValueMayContainEquals) {
  auto c = resolve_search_config(mk("searxng", {{"endpoint", "http://x"}}));
  ASSERT_EQ(c.extra_query, "format=json");
  const std::string tail = to_argv_kv(c);
  EXPECT_NE(tail.find(" extra_query=format=json"), std::string::npos);
  // parse splits on the FIRST '=' only
  std::string tok = "extra_query=format=json";
  char* argv[] = {const_cast<char*>("bin"), tok.data()};
  auto back = parse_argv_kv(2, argv);
  EXPECT_EQ(back.extra_query, "format=json");
}

TEST(SearchConfig, WhitespaceValueThrowsAndUnknownKeyIgnored) {
  auto c = resolve_search_config(mk("http", {{"endpoint", "https://api.example/v1"}}));
  c.title_key = "has space";
  EXPECT_THROW(to_argv_kv(c), MalConfig);
  std::string a = "unknown_key=zzz", b = "notakv";
  char* argv[] = {const_cast<char*>("bin"), a.data(), b.data()};
  auto back = parse_argv_kv(3, argv);      // must not throw
  EXPECT_EQ(back.query_param, "q");        // defaults intact
}
```

- [ ] **Step 2: CMake + run — expect FAIL (missing header).** In `CMakeLists.txt`, next to the other test-source lines (near `tests/test_html_text.cpp`), add:

```cmake
target_sources(hades_tests PRIVATE tests/test_search_config.cpp)
```

Run: `nix develop --command cmake --build build` → compile error.

- [ ] **Step 3: Implement** `include/hades/search/search_config.h`:

```cpp
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
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R SearchConfig` → all pass. Full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/search/search_config.h tests/test_search_config.cpp CMakeLists.txt
git commit -m "feat: SearchConfig header — searxng/brave/http presets, resolver, argv k=v codec"
```

---

## Task 2: `web_search` tool binary + loopback tests

**Files:**
- Create: `tools/web_search_main.cpp`
- Test: `tests/test_web_search_tool.cpp`
- Modify: `CMakeLists.txt`, `package.nix` (bins list — the Pi static build ships every tool binary; forgetting this was a session-search final-review finding)

**Interfaces:**
- Consumes: `hades::SearchConfig` + `hades::parse_argv_kv(argc, argv)` (Task 1, header-only).
- Produces: binary `hades-web-search`; protocol `{"call":"describe"|"web_search","args":{query,max_results?}}` → one JSON line `{ok, result:{results:[{title,url,snippet}], provider}}`. CMake compile-def `WEB_SEARCH_BIN` for tests.

- [ ] **Step 1: Write the failing tests** `tests/test_web_search_tool.cpp`:

```cpp
// tests/test_web_search_tool.cpp — drive hades-web-search against loopback httplib fakes
//
// Config reaches the tool as argv k=v (the wiring contract); the API key reaches it via
// env (never argv). Fakes are SHAPED like the real backends: a SearXNG /search JSON API
// and a Brave-style nested /web/results with a required auth header.
#include <gtest/gtest.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
struct StubSearch {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  std::mutex mu;
  std::string seen_auth, seen_q, seen_format, seen_body;
  StubSearch() {
    srv.Get("/search", [this](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard<std::mutex> l(mu);
        seen_q = req.get_param_value("q");
        seen_format = req.get_param_value("format");
      }
      nlohmann::json results = nlohmann::json::array();
      for (int i = 0; i < 8; ++i)
        results.push_back({{"title", "T" + std::to_string(i)},
                           {"url", "https://r.example/" + std::to_string(i)},
                           {"content", "snippet " + std::to_string(i)}});
      results.push_back({{"content", "no url or title — must be skipped"}});
      res.set_content(nlohmann::json{{"results", results}}.dump(), "application/json");
    });
    srv.Get("/braveish", [this](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard<std::mutex> l(mu);
        seen_auth = req.get_header_value("X-Subscription-Token");
      }
      res.set_content(
          nlohmann::json{
              {"web",
               {{"results",
                 {{{"title", "B"}, {"url", "https://b.example/"}, {"description", "bdesc"}}}}}}}
              .dump(),
          "application/json");
    });
    srv.Post("/postsearch", [this](const httplib::Request& req, httplib::Response& res) {
      {
        std::lock_guard<std::mutex> l(mu);
        seen_body = req.body;
      }
      res.set_content(
          nlohmann::json{{"results", {{{"title", "P"}, {"url", "https://p.example/"}}}}}.dump(),
          "application/json");
    });
    srv.Get("/forbidden", [](const httplib::Request&, httplib::Response& res) {
      res.status = 403;
      res.set_content("denied", "text/plain");
    });
    srv.Get("/garbage", [](const httplib::Request&, httplib::Response& res) {
      res.set_content("not json at all {", "application/json");
    });
    srv.Get("/empty", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"results":[]})", "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~StubSearch() {
    srv.stop();
    th.join();
  }
  std::string base() { return "http://127.0.0.1:" + std::to_string(port); }
  std::string q() { std::lock_guard<std::mutex> l(mu); return seen_q; }
  std::string format() { std::lock_guard<std::mutex> l(mu); return seen_format; }
  std::string auth() { std::lock_guard<std::mutex> l(mu); return seen_auth; }
  std::string body() { std::lock_guard<std::mutex> l(mu); return seen_body; }
};

nlohmann::json call(const std::vector<std::string>& argv_tail, const nlohmann::json& args) {
  std::vector<std::string> argv{WEB_SEARCH_BIN};
  for (const auto& a : argv_tail) argv.push_back(a);
  nlohmann::json req{{"call", "web_search"}, {"args", args}};
  ProcResult r = run_subprocess(argv, req.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(WebSearchTool, SearxngShapeHappyPathAndDefaultClamp) {
  StubSearch web;
  auto j = call({"provider=searxng", "endpoint=" + web.base(),
                 "extra_query=format=json", "snippet_key=content"},
                {{"query", "hades agent"}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false)) << j.dump();
  EXPECT_EQ(web.q(), "hades agent");            // query URL-encoded and delivered
  EXPECT_EQ(web.format(), "json");              // extra_query applied
  const auto& res = j["result"]["results"];
  ASSERT_EQ(res.size(), 5u);                    // default max_results, clamped client-side
  EXPECT_EQ(res[0].value("title", ""), "T0");
  EXPECT_EQ(res[0].value("url", ""), "https://r.example/0");
  EXPECT_EQ(res[0].value("snippet", ""), "snippet 0");
  EXPECT_EQ(j["result"].value("provider", ""), "searxng");
}

TEST(WebSearchTool, MaxResultsArgHonoredAndClamped) {
  StubSearch web;
  auto base = std::vector<std::string>{"provider=searxng", "endpoint=" + web.base(),
                                       "snippet_key=content"};
  auto j3 = call(base, {{"query", "x"}, {"max_results", 3}});
  ASSERT_TRUE(j3.value("ok", false));
  EXPECT_EQ(j3["result"]["results"].size(), 3u);
  auto j50 = call(base, {{"query", "x"}, {"max_results", 50}});
  ASSERT_TRUE(j50.value("ok", false));
  EXPECT_EQ(j50["result"]["results"].size(), 8u);   // clamp is 10; fake serves 8 valid
}

TEST(WebSearchTool, BraveShapeSendsAuthHeaderFromEnv) {
  StubSearch web;
  ::setenv("TEST_SEARCH_KEY", "s3kr1t", 1);
  auto j = call({"provider=http", "endpoint=" + web.base() + "/braveish",
                 "api_key_env=TEST_SEARCH_KEY", "auth_header=X-Subscription-Token",
                 "auth_scheme=none", "results_path=/web/results", "snippet_key=description"},
                {{"query", "x"}});
  ::unsetenv("TEST_SEARCH_KEY");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(web.auth(), "s3kr1t");              // raw key, no Bearer prefix
  const auto& res = j["result"]["results"];
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0].value("snippet", ""), "bdesc");
}

TEST(WebSearchTool, MissingKeyEnvFailsClosed) {
  StubSearch web;
  ::unsetenv("TEST_SEARCH_KEY_ABSENT");
  auto j = call({"provider=http", "endpoint=" + web.base() + "/braveish",
                 "api_key_env=TEST_SEARCH_KEY_ABSENT"},
                {{"query", "x"}});
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(WebSearchTool, PostMethodSendsJsonBody) {
  StubSearch web;
  auto j = call({"provider=http", "endpoint=" + web.base() + "/postsearch",
                 "method=post", "query_param=q"},
                {{"query", "post me"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  auto body = nlohmann::json::parse(web.body(), nullptr, false);
  ASSERT_TRUE(body.is_object());
  EXPECT_EQ(body.value("q", ""), "post me");
  EXPECT_EQ(j["result"]["results"][0].value("title", ""), "P");
  EXPECT_EQ(j["result"]["results"][0].value("snippet", ""), "");   // missing -> empty
}

TEST(WebSearchTool, ErrorPathsFailSoft) {
  StubSearch web;
  auto forbidden = call({"provider=http", "endpoint=" + web.base() + "/forbidden"},
                        {{"query", "x"}});
  EXPECT_FALSE(forbidden.value("ok", true));
  EXPECT_EQ(forbidden["result"].value("status", 0), 403);
  auto garbage = call({"provider=http", "endpoint=" + web.base() + "/garbage"},
                      {{"query", "x"}});
  EXPECT_FALSE(garbage.value("ok", true));
  auto badpath = call({"provider=http", "endpoint=" + web.base() + "/empty",
                       "results_path=/nope"},
                      {{"query", "x"}});
  EXPECT_FALSE(badpath.value("ok", true));
  auto down = call({"provider=http", "endpoint=http://127.0.0.1:1", "timeout_s=2"},
                   {{"query", "x"}});
  EXPECT_FALSE(down.value("ok", true));
}

TEST(WebSearchTool, ZeroHitsIsOkEmpty) {
  StubSearch web;
  auto j = call({"provider=http", "endpoint=" + web.base() + "/empty"}, {{"query", "x"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_TRUE(j["result"]["results"].empty());
}

TEST(WebSearchTool, EmptyOrNonStringQueryFailsClosed) {
  StubSearch web;
  auto base = std::vector<std::string>{"provider=http", "endpoint=" + web.base() + "/empty"};
  EXPECT_FALSE(call(base, {{"query", ""}}).value("ok", true));
  EXPECT_FALSE(call(base, {{"query", 42}}).value("ok", true));
  EXPECT_FALSE(call(base, nlohmann::json::object()).value("ok", true));
}

TEST(WebSearchTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({WEB_SEARCH_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "web_search");
  EXPECT_TRUE(j["result"]["schema"]["properties"].contains("query"));
  EXPECT_TRUE(j["result"]["schema"]["properties"].contains("max_results"));
}
```

- [ ] **Step 2: CMake + package.nix + run — expect FAIL.** In `CMakeLists.txt`, after the `hades-http-fetch` block (~line 85), add:

```cmake
add_executable(hades-web-search tools/web_search_main.cpp)
target_link_libraries(hades-web-search PRIVATE cpr::cpr nlohmann_json::nlohmann_json)
target_include_directories(hades-web-search PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

Next to the other test sources add:

```cmake
target_sources(hades_tests PRIVATE tests/test_web_search_tool.cpp)
```

Next to the existing `HTTP_FETCH_BIN` compile-def (~line 105-108) add:

```cmake
target_compile_definitions(hades_tests PRIVATE WEB_SEARCH_BIN="$<TARGET_FILE:hades-web-search>")
```

and append `hades-web-search` to that `add_dependencies(hades_tests …)` line. In `package.nix`, add `"hades-web-search"` to the bins list (grep for `"hades-session-search"` and add alongside).

Run: `nix develop --command cmake --build build` → compile FAIL (`tools/web_search_main.cpp` missing).

- [ ] **Step 3: Implement** `tools/web_search_main.cpp`:

```cpp
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
              for (const auto& e : j.at(ptr)) {
                if (static_cast<int>(hits.size()) >= max_results) break;
                if (!e.is_object()) continue;
                const std::string title = e.value(cfg.title_key, "");
                const std::string href = e.value(cfg.url_key, "");
                if (title.empty() || href.empty()) continue;   // spec: skip incomplete
                std::string snippet = e.value(cfg.snippet_key, "");
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
```

- [ ] **Step 4: Build + test.** `-R WebSearchTool` → all pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/web_search_main.cpp tests/test_web_search_tool.cpp CMakeLists.txt package.nix
git commit -m "feat: web_search native tool — generic engine, loopback-tested searxng/brave/post shapes"
```

---

## Task 3: Capability row + wiring + redaction

**Files:**
- Modify: `include/hades/objective/capability_policy.h` (enum), `src/behaviors/capability_policy.cpp` (`capability_of` row + veto case), `app/agent_wiring.h`/`.cpp` (Search block param + resolve + argv), `app/hades_main.cpp` (redaction)
- Test: `tests/test_web_search_wiring.cpp` (create), `tests/test_capability_policy.cpp` (append)
- Modify: `CMakeLists.txt` (one test-source line)

**Interfaces:**
- Consumes: `resolve_search_config` / `to_argv_kv` (Task 1), `hades-web-search` binary + `WEB_SEARCH_BIN` def (Task 2).
- Produces: `Capability::WebSearch` (enum, before `Unknown`); `capability_of("web_search") == WebSearch` → `allow()`; `wire_agent` trailing param `const Block& search_cfg = Block{}`; Manifest overload extracts `m.of("Search")`.

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_capability_policy.cpp` (match file style):

```cpp
TEST(CapabilityPolicy, WebSearchIsAllowed) {
  EXPECT_EQ(CapabilityPolicy::capability_of("web_search"), Capability::WebSearch);
  CapabilityScope sc;              // defaults: confirm_unscoped — proves NOT Unknown->confirm
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action a{Action::Kind::ToolCall};
  a.tool = "web_search";
  a.args = {{"query", "anything"}};
  EXPECT_FALSE(p.veto(bb, a).vetoed);
}
```

Create `tests/test_web_search_wiring.cpp`:

```cpp
// tests/test_web_search_wiring.cpp — wiring pins the resolved Search config via argv k=v
// Roster has NO llm module (session_search-wiring precedent): the ToolRunner runs the REAL
// binary against a loopback stub; we drive TOOL_REQUEST directly.
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

namespace {
struct StubSearxng {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  StubSearxng() {
    srv.Get("/search", [](const httplib::Request& req, httplib::Response& res) {
      if (req.get_param_value("format") != "json") {   // real SearXNG JSON contract
        res.status = 403;
        return;
      }
      res.set_content(
          nlohmann::json{{"results",
                          {{{"title", "Hit"},
                            {"url", "https://hit.example/"},
                            {"content", "found it"}}}}}
              .dump(),
          "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~StubSearxng() {
    srv.stop();
    th.join();
  }
};

std::string manifest_text(const std::string& search_block) {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\nModule = arbiter\n" +
         "Tool = web_search { native = " + WEB_SEARCH_BIN + " }\n" + search_block;
}
}  // namespace

TEST(WebSearchWiring, ArgvCarriesResolvedConfigEndToEnd) {
  StubSearxng stub;
  const std::string manifest = manifest_text(
      "Search\n{\n  provider = searxng\n  endpoint = http://127.0.0.1:" +
      std::to_string(stub.port) + "\n}\n");
  Blackboard bb;
  Manifest m = parse_manifest(manifest);
  Agent agent = build_agent(bb, m);
  nlohmann::json result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb.post("TOOL_REQUEST",
          {{"id", "w1"}, {"tool", "web_search"}, {"args", {{"query", "needle"}}}},
          "arbiter");
  bb.pump();
  ASSERT_TRUE(result.is_object());
  ASSERT_TRUE(result.value("ok", false)) << result.dump();
  const auto& hits = result["content"]["results"];
  ASSERT_EQ(hits.size(), 1u) << result.dump();
  EXPECT_EQ(hits[0].value("title", ""), "Hit");         // preset extra_query=format=json worked
  EXPECT_EQ(hits[0].value("snippet", ""), "found it");  // preset snippet_key=content worked
}

TEST(WebSearchWiring, MissingSearchBlockThrowsMalConfig) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(""));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(WebSearchWiring, MissingEndpointThrowsMalConfig) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text("Search\n{\n  provider = searxng\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(WebSearchWiring, UnsetKeyEnvThrowsMalConfig) {
  ::unsetenv("WS_WIRE_KEY_ABSENT");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(
      "Search\n{\n  provider = http\n  endpoint = http://127.0.0.1:1\n"
      "  api_key_env = WS_WIRE_KEY_ABSENT\n}\n"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(WebSearchWiring, NoWebSearchToolIgnoresSearchBlock) {
  // A Search block without the tool rostered is inert — no MalConfig, no tool announced.
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n"
      "Search\n{\n  provider = searxng\n  endpoint = http://127.0.0.1:1\n}\n");
  Agent agent = build_agent(bb, m);   // must not throw
  SUCCEED();
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** Add next to the other wiring test sources:

```cmake
target_sources(hades_tests PRIVATE tests/test_web_search_wiring.cpp)
```

Build → compile FAIL on the capability test (`Capability::WebSearch` missing).

- [ ] **Step 3: Implement.**

1. `include/hades/objective/capability_policy.h` — enum becomes (add `WebSearch` before `Unknown`):

```cpp
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SessionRead, SkillRead, SkillWrite, PeerAsk, GitRead, ExecScoped, SelfSchedule, McpTool, WebSearch, Unknown };
```

2. `src/behaviors/capability_policy.cpp` — in `capability_of`, after the `session_search` row:

```cpp
  if (tool == "web_search")                              return Capability::WebSearch;
```

In the `veto` switch, after `case Capability::SessionRead:`'s block:

```cpp
    case Capability::WebSearch:
      // Endpoint is operator-pinned via argv (the mcp_url precedent: operator-set
      // endpoints are exempt from the private-net gate — a loopback/LAN SearXNG works);
      // the LLM supplies only query text, so there is no SSRF surface in the args.
      // Peer/heartbeat turns can search unattended: exposure is query text flowing to
      // the operator-chosen backend only (per-origin scopes = capability v2).
      return allow();
```

3. `app/agent_wiring.h` — add trailing parameter to `wire_agent`'s declaration (AFTER every existing parameter, keeping all existing call sites valid): `const Block& search_cfg = Block{}`.

4. `app/agent_wiring.cpp` — in `wire_agent`, next to the other path resolutions (after the sessions_dir block, ~line 211), add:

```cpp
  // web_search: resolve the Search block (presets + overrides) and pin the FULL provider
  // config via argv k=v — single source of truth, the LLM can never redirect the endpoint.
  // The API key stays OUT of argv (/proc-visible): only the env var NAME travels, and its
  // PRESENCE is checked here so a missing key fails loud at boot, not mid-turn.
  bool has_web_search = false;
  for (const auto& t : tools)
    if (t.name == "web_search" && t.kv.count("native")) has_web_search = true;
  std::string search_argv;
  if (has_web_search) {
    if (search_cfg.section.empty())
      throw MalConfig("Tool web_search requires a Search block");
    const SearchConfig sc = resolve_search_config(search_cfg);
    if (!sc.api_key_env.empty()) {
      const char* k = std::getenv(sc.api_key_env.c_str());
      if (!k || !*k)
        throw MalConfig("Search api_key_env " + sc.api_key_env + " is not set");
    }
    search_argv = to_argv_kv(sc);   // values whitespace-rejected inside
  }
```

Add `#include "hades/search/search_config.h"` and `#include <cstdlib>` (if absent) to the file's includes. In the `tools_resolved` loop, after the `session_search` branch:

```cpp
    else if (t.name == "web_search" && t.kv.count("native"))
      t.kv["native"] += search_argv;
```

5. Manifest overload (~line 663, next to the Skills extraction):

```cpp
  const auto search_blocks = m.of("Search");
  const Block search_cfg = search_blocks.empty() ? Block{} : search_blocks.front();
```

and pass `search_cfg` as the new last argument of the `wire_agent(...)` call (~line 695). The TEST overload's `wire_agent` call (~line 588) is UNCHANGED (default `Block{}` → feature absent).

6. `app/hades_main.cpp` — after the bridge-secret redaction block (~line 137), add:

```cpp
    // Redact the web-search API key too (it travels in a request header). Best-effort:
    // resolve the same env var the tool will use (Search.api_key_env or its preset default).
    {
      const auto se = manifest.of("Search");
      std::string se_env = "HADES_SEARCH_KEY";
      if (!se.empty() && se.front().kv.count("api_key_env"))
        se_env = se.front().kv.at("api_key_env");
      if (const char* se_key = std::getenv(se_env.c_str()); se_key && *se_key)
        eventlog.add_redaction(se_key);
    }
```

- [ ] **Step 4: Build + test.** `-R "WebSearchWiring|CapabilityPolicy"` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/objective/capability_policy.h src/behaviors/capability_policy.cpp \
        app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp \
        tests/test_web_search_wiring.cpp tests/test_capability_policy.cpp CMakeLists.txt
git commit -m "feat: wire web_search — Search block, WebSearch capability (allow), argv k=v pin, key redaction"
```

---

## Task 4: Ship — docs, dev.hades, .env.example

**Files:**
- Modify: `docs/manifest-reference.md`, `manifests/dev.hades`, `.env.example`, `CLAUDE.md`, `README.md`

- [ ] **Step 1: manifest-reference.** Three edits:

(a) New numbered section at the end (check the current last `## N.` number and continue it):

```markdown
## N. Search block — `web_search`

Backing config for the opt-in `web_search` native tool (roster `Tool = web_search` + this
block; the tool line without the block is a boot `MalConfig`). One generic HTTP engine;
`provider` presets fill the knobs, any explicit key overrides its preset value.

| key | meaning | default |
|---|---|---|
| `provider` | `searxng` \| `brave` \| `http` | REQUIRED |
| `endpoint` | base URL — searxng: instance base (`/search` appended); brave/http: full request URL | brave preset only |
| `api_key_env` | env var NAME holding the key; empty = no auth. The key value is redacted in `session.log`. | preset (`HADES_SEARCH_KEY` for brave; empty otherwise) |
| `method` | `get` \| `post` (post body = `{query_param: query}`) | `get` |
| `query_param` | URL param (get) / JSON body key (post) carrying the query | `q` |
| `auth_header` / `auth_scheme` | header carrying the key; `bearer` prefixes `Bearer `, `none` sends the raw key | `Authorization` / `bearer` |
| `results_path` | JSON pointer to the results array | `/results` (brave: `/web/results`) |
| `title_key` / `url_key` / `snippet_key` | per-result field names | `title`/`url`/`snippet` (searxng snippet: `content`; brave: `description`) |
| `extra_query` | one literal `k=v` appended to a GET query (searxng preset: `format=json`) | empty |
| `max_results` | default result count (clamp 1..10; per-call `max_results` arg can lower/raise within the clamp) | `5` |
| `timeout_s` | per-request HTTP timeout | `15` |

SearXNG worked example (self-hosted, loopback):

```
Tool = web_search { native = ./build/hades-web-search }

Search
{
  provider = searxng
  endpoint = http://127.0.0.1:8888
}
```

**Gotchas:** the SearXNG instance must enable the JSON API — `search: formats: [html, json]`
in its `settings.yml`, else every query 403s (the tool's error says so). `endpoint` is the
BASE url for searxng (house footgun family — the tool appends `/search`). Boot fails loud
(`MalConfig`) on: missing `Search` block, missing `endpoint`, `api_key_env` naming an unset
env var, any value containing whitespace. Snippets are byte-capped at 500; zero hits is
`ok` with an empty list, not an error.
```

(b) §4 argv-append table — add a row:

```markdown
| `web_search` | full resolved provider config as `k=v` pairs | `Search` block (§N) | requires the `Search` block (else `MalConfig`); the API key is NEVER in argv — only the env var NAME travels |
```

(c) §5 capability table — add a row after the `session_search` one:

```markdown
| `web_search` | WebSearch | **always allow** — endpoint operator-pinned via argv (mcp_url precedent; loopback/LAN SearXNG works); only the query text is LLM-chosen. Peer/heartbeat turns can search unattended (query flows to the operator-chosen backend only). |
```

- [ ] **Step 2: dev.hades.** Add a COMMENTED block next to the other commented optional features (match the file's comment style, `#` prefix):

```
# Web search (opt-in): self-hosted SearXNG (enable json in its settings.yml) or brave/http.
# Tool = web_search { native = ./build/hades-web-search }
# Search
# {
#   provider = searxng
#   endpoint = http://127.0.0.1:8888
# }
```

- [ ] **Step 3: .env.example.** Add with the other commented optional keys:

```
# Web search API key (only for Search providers that need one, e.g. provider = brave;
# a self-hosted SearXNG needs no key):
# HADES_SEARCH_KEY=
```

- [ ] **Step 4: CLAUDE.md + README.** CLAUDE.md: CC tool-gap item 2 → `~~**\`web_search\` tool**~~ — **SHIPPED 2026-07-18** (\`feat/web-search\`): generic engine + searxng/brave presets + raw http knobs; Search block; WebSearch capability allow; key env-only + redacted.`; current-state header: tool count **19 → 20 tools** (add `web_search` to the tool list there) and test count → the Task 3 full-suite total. README.md: `grep -n "19 tools\|19 native" README.md` and bump any tool/test counts to match.
- [ ] **Step 5: Full verify both lanes.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan
```

Expected: identical full-green counts on both.
- [ ] **Step 6: Commit.**

```bash
git add docs/manifest-reference.md manifests/dev.hades .env.example CLAUDE.md README.md
git commit -m "docs: ship web_search — Search block reference, dev.hades example, .env.example, counts"
```

---

## Verification (end-to-end)

1. Full suite both lanes, all green (~28 new tests over the 715 baseline).
2. Manual live smoke (Vaios, needs a SearXNG instance with json enabled): uncomment the
   dev.local.hades block → "search the web for MOOS-IvP" → hits with snippets → "open the
   first result" → `http_fetch` returns extracted text (the compose story).
3. Security spot-check: `ps aux | grep hades-web-search` during a call shows NO key in
   argv; `hades-scope session.log` shows the key value redacted if it ever appears.

## Execution

Subagent-driven development (house process): fresh implementer per task, per-task review,
final whole-branch review, then finishing-a-development-branch (merge ff to main — push
only on Vaios's word).
