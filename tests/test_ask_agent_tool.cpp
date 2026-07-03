// tests/test_ask_agent_tool.cpp — drive the hades-ask-agent binary against a stub peer
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "hades/bridge/protocol.h"
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
// Stub peer bridge: answers /ask like a real BridgeModule would.
struct StubPeer {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  std::string seen_secret, seen_body;
  std::string reply_body = R"({"ok":true,"reply":"42 GB free"})";  // overridable per test
  StubPeer() {
    srv.Post("/ask", [this](const httplib::Request& req, httplib::Response& res) {
      seen_secret = req.get_header_value("X-Hades-Bridge");
      seen_body = req.body;
      if (seen_secret != "s3cret") {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"forbidden"})", "application/json");
        return;
      }
      res.set_content(reply_body, "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
  }
  ~StubPeer() { srv.stop(); th.join(); }
};

nlohmann::json run_tool(const std::vector<std::string>& argv, const std::string& stdin_line) {
  ProcResult r = run_subprocess(argv, stdin_line, 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
std::string call(const std::string& peer, const std::string& msg) {
  return nlohmann::json{{"call", "ask_agent"}, {"args", {{"peer", peer}, {"message", msg}}}}
      .dump();
}
}  // namespace

TEST(AskAgentTool, DescribeYieldsSpecWithPeerRoster) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                     "worker1=http://127.0.0.1:1"},
                    R"({"call":"describe"})");
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "ask_agent");
  // The description names the known peers so the LLM can pick one.
  EXPECT_NE(j["result"].value("description", "").find("worker1"), std::string::npos);
}

TEST(AskAgentTool, AsksPeerAndReturnsReply) {
  StubPeer peer;
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "10",
                     "worker1=http://127.0.0.1:" + std::to_string(peer.port)},
                    call("worker1", "disk space?"));
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("reply", ""), "42 GB free");
  EXPECT_EQ(peer.seen_secret, "s3cret");
  auto sent = parse_ask(peer.seen_body);
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.from, "front");
  EXPECT_EQ(sent.hops, 0);
  EXPECT_EQ(sent.message, "disk space?");
}

TEST(AskAgentTool, UnknownPeerFailsClosed) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                     "worker1=http://127.0.0.1:1"},
                    call("ghost", "hi"));
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("unknown peer"), std::string::npos);
}

TEST(AskAgentTool, PeerDownReturnsError) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "2",
                     "worker1=http://127.0.0.1:1"},          // nothing listens on port 1
                    call("worker1", "hi"));
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(AskAgentTool, MissingSecretEnvFailsClosed) {
  ::unsetenv("HADES_TEST_ASK_SECRET_MISSING");
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET_MISSING", "5",
                     "worker1=http://127.0.0.1:1"},
                    call("worker1", "hi"));
  EXPECT_FALSE(j.value("ok", true));
}

// A peer returning a body whose keys exist but have the WRONG type must not abort the tool
// (.value() throws type_error.302 on a type mismatch). Every case must yield a clean ok:false.
TEST(AskAgentTool, MalformedPeerResponseFailsClosed) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  for (const char* body : {R"({"ok":1})",                  // ok is a number, not a bool
                           R"({"ok":true,"reply":42})",    // reply is a number, not a string
                           R"({"ok":"true"})",             // ok is a string, not a bool
                           "not json"}) {                  // not JSON at all
    StubPeer peer;
    peer.reply_body = body;
    auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "10",
                       "worker1=http://127.0.0.1:" + std::to_string(peer.port)},
                      call("worker1", "disk space?"));
    ASSERT_FALSE(j.is_discarded()) << body;
    EXPECT_FALSE(j.value("ok", true)) << body;
  }
}

TEST(AskAgentTool, MalformedArgsFailClosed) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  const std::vector<std::string> argv = {ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                                         "worker1=http://127.0.0.1:1"};
  for (const char* raw :
       {R"({"call":"ask_agent","args":{"peer":"worker1"}})",              // no message
        R"({"call":"ask_agent","args":{"message":"m"}})",                 // no peer
        R"({"call":"ask_agent","args":{"peer":7,"message":"m"}})",        // non-string
        R"({"call":"nonsense"})"}) {
    auto j = run_tool(argv, raw);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
