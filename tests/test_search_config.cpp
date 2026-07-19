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
