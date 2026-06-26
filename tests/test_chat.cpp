#include <gtest/gtest.h>
#include <sstream>
#include "hades/module/chat_module.h"
#include "hades/blackboard.h"
using namespace hades;

TEST(Chat, ReplPostsUserAndPrintsAssistant) {
  Blackboard bb; ChatModule c; c.on_attach(bb);
  // echo agent: turn USER_MESSAGE into ASSISTANT_MESSAGE
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "test");
  });
  std::istringstream in("hello\n/quit\n"); std::ostringstream out;
  c.run_repl(in, out);
  EXPECT_NE(out.str().find("echo:hello"), std::string::npos);
}

TEST(Chat, ConfirmPromptReadsYesFromStdin) {
  Blackboard bb; ChatModule c; c.on_attach(bb);
  nlohmann::json resp;
  bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) { resp = e.value; });
  // a fake agent: on USER_MESSAGE, raise a confirm request
  bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "ok?"}}, "arb");
  });
  std::istringstream in("go\ny\n/quit\n"); std::ostringstream out;
  // "go" -> USER_MESSAGE -> CONFIRM_REQUEST; "y" answers the confirm; "/quit" ends
  c.run_repl(in, out);
  ASSERT_FALSE(resp.is_null());
  EXPECT_EQ(resp["id"], "c1");
  EXPECT_TRUE(resp["approved"].get<bool>());
}
