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
