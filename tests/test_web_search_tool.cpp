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
