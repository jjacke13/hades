// tests/test_chat.cpp — unit tests for ChatModule stdin/stdout REPL on the Blackboard
//
// Verifies that run_repl() posts USER_MESSAGE for each input line, prints
// ASSISTANT_MESSAGE replies to the output stream, and correctly handles
// CONFIRM_REQUEST by prompting inline and posting a CONFIRM_RESPONSE — the
// human-facing I/O layer that drives the Arbiter and receives its answers.

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>
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
  c.set_turn_timeout_s(0.02);  // force a fast abandonment instead of the 900s default
  std::istringstream in("hang\n/quit\n"); std::ostringstream out;
  c.run_repl(in, out);
  EXPECT_EQ(abandoned, 1);
  EXPECT_NE(out.str().find("[timed out]"), std::string::npos);
}

TEST(Chat, ConfirmPromptReadsYesFromStdin) {
  Blackboard bb; ChatModule c; c.on_attach(bb);
  // The fake agent below never posts ASSISTANT_MESSAGE, so the turn never completes and
  // run_until waits out its idle timeout. The confirm 'y' is read inline during the first
  // pump (before any timeout), so a tiny test-only timeout keeps the assertions valid while
  // cutting the wall-clock from the production default (900s) to milliseconds. The fast timeout
  // does post TURN_ABANDONED and print [timed out], but nothing here asserts on that output.
  c.set_turn_timeout_s(0.05);
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

// `/new` starts a fresh session mid-run: the REPL intercepts it (alongside `/quit`) and posts a
// NEW_SESSION bus message — it must NOT be turned into a USER_MESSAGE (which would feed "/new" to
// the LLM). Drive "/new\n/quit\n": expect exactly one NEW_SESSION and no USER_MESSAGE == "/new".
TEST(Chat, SlashNewPostsNewSession) {
  Blackboard bb; ChatModule c; c.on_attach(bb);
  int new_session = 0;
  std::vector<std::string> user_msgs;
  bb.subscribe("NEW_SESSION", [&](const Entry&) { ++new_session; });
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    if (e.value.is_string()) user_msgs.push_back(e.value.get<std::string>());
  });
  std::istringstream in("/new\n/quit\n"); std::ostringstream out;
  c.run_repl(in, out);
  EXPECT_EQ(new_session, 1);                              // /new posted NEW_SESSION exactly once
  for (const auto& m : user_msgs) EXPECT_NE(m, "/new");   // /new was NOT posted as a USER_MESSAGE
}
