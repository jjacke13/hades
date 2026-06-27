// tests/test_serve.cpp — HttpServerModule turn-driving (socket-free)
//
// Exercises handle_message/handle_confirm through the real agent graph (build_agent
// with a scripted Provider), with no sockets: a plain answer returns {reply}, and a
// destructive tool call returns {needs_confirm} then resolves via handle_confirm.

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
using namespace hades;

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
