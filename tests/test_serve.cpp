// tests/test_serve.cpp — HttpServerModule turn-driving (socket-free)
//
// Exercises handle_message/handle_confirm through the real agent graph (build_agent
// with a scripted Provider), with no sockets: a plain answer returns {reply}, and a
// destructive tool call returns {needs_confirm} then resolves via handle_confirm.

#include <gtest/gtest.h>
#include <httplib.h>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/module/http_server_module.h"
#include "hades/timeouts.h"  // kDefaultTurnIdleTimeoutS
using namespace hades;

namespace {
// Probe subclass: httplib::Server's socket-timeout members are protected with no public
// getter, so a derived class is the only way to observe what configure_server_ set.
struct ProbeServer : httplib::Server {
  time_t read_sec() const { return read_timeout_sec_; }
  time_t write_sec() const { return write_timeout_sec_; }
};
}  // namespace

namespace {
struct TextProvider : Provider {  // always answers with fixed text
  std::string text;
  explicit TextProvider(std::string t) : text(std::move(t)) {}
  LlmResponse complete(const LlmRequest&) override {
    LlmResponse r;
    r.text = text;
    return r;
  }
};
struct ShellCallProvider : Provider {  // first call -> a destructive shell tool_call
  int n = 0;
  LlmResponse complete(const LlmRequest&) override {
    LlmResponse r;
    if (n++ == 0)
      r.tool_call = {{"id", "c1"}, {"name", "shell"}, {"arguments", {{"cmd", "rm -rf /"}}}};
    else
      r.text = "done";
    return r;
  }
};
}  // namespace

TEST(HttpServer, ChatReturnsReply) {
  Blackboard bb;
  auto agent = build_agent(bb, std::make_unique<TextProvider>("hi there"), {}, {}, "m");
  auto out = agent.serve->handle_message("hello");
  EXPECT_EQ(out.value("reply", ""), "hi there");
}

TEST(HttpServer, ChatTimeoutPostsAbandoned) {
  // No Arbiter attached -> USER_MESSAGE has no handler, so the turn never produces an
  // ASSISTANT_MESSAGE or CONFIRM_REQUEST: collect_'s run_until hits the (tiny, test-only)
  // idle timeout. handle_message must post TURN_ABANDONED and return the [timed out] reply.
  Blackboard bb;
  HttpServerModule srv;
  srv.on_attach(bb);
  srv.set_collect_timeout_s(0.02);  // force a fast abandonment instead of the 180s default
  int abandoned = 0;
  bb.subscribe("TURN_ABANDONED", [&](const Entry&) { ++abandoned; });
  auto out = srv.handle_message("hang");
  EXPECT_EQ(out.value("reply", ""), "[timed out]");
  EXPECT_EQ(abandoned, 1);
}

TEST(HttpServer, DestructiveCallNeedsConfirmThenDeclined) {
  Blackboard bb;
  Block obj;
  obj.section = "Objective";
  obj.name = "avoid_destructive";
  auto agent = build_agent(bb, std::make_unique<ShellCallProvider>(), {}, {obj}, "m");

  auto out = agent.serve->handle_message("wipe the disk");
  ASSERT_TRUE(out.value("needs_confirm", false));
  EXPECT_FALSE(out.value("id", "").empty());

  auto declined = agent.serve->handle_confirm(out.value("id", ""), false);
  EXPECT_EQ(declined.value("reply", ""), "[declined by user]");
}

TEST(HttpServer, IdleTimeoutTracksConfiguredValue) {
  // The effective idle ceiling that feeds both collect_ and the socket timeouts must
  // track the configured member (default 900, overridable), not a hardcoded literal.
  HttpServerModule srv;
  EXPECT_DOUBLE_EQ(srv.idle_timeout_s(), kDefaultTurnIdleTimeoutS);  // absent key -> 900
  srv.set_collect_timeout_s(30.0);
  EXPECT_DOUBLE_EQ(srv.idle_timeout_s(), 30.0);  // manifest/test override wins
}

TEST(HttpServer, ConfigureServerRaisesSocketTimeoutsAboveIdle) {
  // configure_server_ must set read/write socket timeouts strictly above the idle ceiling
  // so a long (up-to-idle) collect_ handler's connection is never dropped mid-flight.
  ProbeServer srv;
  const double idle = kDefaultTurnIdleTimeoutS;  // 900
  HttpServerModule::configure_server_(srv, idle);
  EXPECT_EQ(srv.read_sec(), static_cast<time_t>(idle) + 60);
  EXPECT_EQ(srv.write_sec(), static_cast<time_t>(idle) + 60);
  EXPECT_GT(srv.read_sec(), static_cast<time_t>(idle));
  EXPECT_GT(srv.write_sec(), static_cast<time_t>(idle));
}
