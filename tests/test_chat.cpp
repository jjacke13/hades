// tests/test_chat.cpp — unit tests for ChatModule stdin/stdout REPL on the Blackboard
//
// Verifies that run_repl() posts USER_MESSAGE for each input line, prints
// ASSISTANT_MESSAGE replies to the output stream, and correctly handles
// CONFIRM_REQUEST by prompting inline and posting a CONFIRM_RESPONSE — the
// human-facing I/O layer that drives the Arbiter and receives its answers.

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

TEST(Chat, ReplTimeoutPostsAbandonedAndPrints) {
  // A turn that NEVER completes: no handler turns USER_MESSAGE into ASSISTANT_MESSAGE,
  // so run_until hits the (tiny, test-only) idle timeout and the turn is abandoned. The
  // REPL must post TURN_ABANDONED (so the Arbiter can bump its turn epoch) and surface
  // [timed out] to the user, then continue to the next prompt.
  Blackboard bb; ChatModule c; c.on_attach(bb);
  int abandoned = 0;
  bb.subscribe("TURN_ABANDONED", [&](const Entry&) { ++abandoned; });
  c.set_turn_timeout_s(0.02);  // force a fast abandonment instead of the 180s default
  std::istringstream in("hang\n/quit\n"); std::ostringstream out;
  c.run_repl(in, out);
  EXPECT_EQ(abandoned, 1);
  EXPECT_NE(out.str().find("[timed out]"), std::string::npos);
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
