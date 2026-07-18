// tests/test_http_fetch_tool.cpp — drive hades-http-fetch against a loopback httplib server
//
// The private-host gate lives in CapabilityPolicy at the Arbiter, NOT in the tool, so the
// binary fetches loopback fine here (the test_ask_agent_tool.cpp precedent). Serves canned
// HTML/JSON; asserts default extraction, the raw escape, and non-HTML passthrough.
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
struct StubWeb {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  StubWeb() {
    srv.Get("/page", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(
          "<html><head><title>T</title><script>var x=1;</script></head>"
          "<body><p>Hello &amp; welcome</p><a href=\"/next\">Next</a></body></html>",
          "text/html");
    });
    srv.Get("/api", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"k":"v"})", "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~StubWeb() {
    srv.stop();
    th.join();
  }
  std::string url(const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }
};

nlohmann::json fetch(const nlohmann::json& args) {
  nlohmann::json req{{"call", "http_fetch"}, {"args", args}};
  ProcResult r = run_subprocess({HTTP_FETCH_BIN}, req.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(HttpFetchTool, HtmlIsExtractedByDefault) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"].value("extracted", false));
  const std::string body = j["result"].value("body", "");
  EXPECT_EQ(body.rfind("T\n", 0), 0u);                          // title first line
  EXPECT_NE(body.find("Hello & welcome"), std::string::npos);   // entity decoded
  EXPECT_NE(body.find("Next (/next)"), std::string::npos);      // link inline
  EXPECT_EQ(body.find("var x=1"), std::string::npos);           // script dropped
}

TEST(HttpFetchTool, RawFlagReturnsUntouchedBody) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}, {"raw", true}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_FALSE(j["result"].value("extracted", true));
  EXPECT_NE(j["result"].value("body", "").find("<script>var x=1;</script>"),
            std::string::npos);
}

TEST(HttpFetchTool, NonHtmlPassesThroughUntouched) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/api")}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_FALSE(j["result"].value("extracted", true));
  EXPECT_EQ(j["result"].value("body", ""), R"({"k":"v"})");
}

TEST(HttpFetchTool, NonBooleanRawTreatedAsAbsent) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}, {"raw", "yes"}});   // string, not bool
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"].value("extracted", false));            // still extracts
}

TEST(HttpFetchTool, DescribeIncludesRawProperty) {
  ProcResult r = run_subprocess({HTTP_FETCH_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"]["schema"]["properties"].contains("raw"));
}
