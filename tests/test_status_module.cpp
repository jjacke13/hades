// tests/test_status_module.cpp — StatusModule: aggregate turn stats onto AGENT_STATUS
//
// The uProcessWatch analog: subscribes the existing turn traffic (LLM_REQUEST/RESPONSE,
// BUDGET_SPENT_USD, USER_MESSAGE, NEW_SESSION) and posts a single AGENT_STATUS latest-value
// the front-ends render — data producer decoupled from the surface (chat prints it today,
// web/telegram can consume the same key later).
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/module/status_module.h"
using namespace hades;

static nlohmann::json status_of(Blackboard& bb) {
  auto e = bb.get("AGENT_STATUS");
  return e ? e->value : nlohmann::json();
}

TEST(StatusModule, AggregatesTokensSpendTurnModel) {
  Blackboard bb;
  StatusModule s;
  s.on_attach(bb);
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.post("LLM_REQUEST", {{"model", "gpt-5.5"}, {"messages", nlohmann::json::array()}}, "arbiter");
  bb.post("LLM_RESPONSE", {{"text", "yo"}, {"prompt_tokens", 12000}, {"completion_tokens", 437}},
          "llm");
  bb.post("BUDGET_SPENT_USD", 0.0372, "llm");
  bb.pump();
  auto st = status_of(bb);
  ASSERT_TRUE(st.is_object());
  EXPECT_EQ(st.value("ctx_tokens", 0), 12437);
  EXPECT_DOUBLE_EQ(st.value("spent_usd", 0.0), 0.0372);
  EXPECT_EQ(st.value("turn", 0), 1);
  EXPECT_EQ(st.value("model", ""), "gpt-5.5");
  const std::string line = st.value("line", "");
  EXPECT_NE(line.find("12.4k tok"), std::string::npos) << line;
  EXPECT_NE(line.find("$0.0372"), std::string::npos) << line;
  EXPECT_NE(line.find("turn 1"), std::string::npos) << line;
  EXPECT_NE(line.find("gpt-5.5"), std::string::npos) << line;
}

TEST(StatusModule, TurnCountsUserMessages) {
  Blackboard bb;
  StatusModule s;
  s.on_attach(bb);
  for (int i = 0; i < 3; ++i) bb.post("USER_MESSAGE", "m", "chat");
  bb.post("LLM_RESPONSE", {{"prompt_tokens", 10}, {"completion_tokens", 1}}, "llm");
  bb.pump();
  EXPECT_EQ(status_of(bb).value("turn", 0), 3);
}

TEST(StatusModule, NewSessionResetsCountsButKeepsSpendAndModel) {
  // Spend is cumulative process-lifetime (the budget objective's view); context and turn
  // describe the CONVERSATION, which /new rotates.
  Blackboard bb;
  StatusModule s;
  s.on_attach(bb);
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.post("LLM_REQUEST", {{"model", "m1"}}, "arbiter");
  bb.post("LLM_RESPONSE", {{"prompt_tokens", 500}, {"completion_tokens", 5}}, "llm");
  bb.post("BUDGET_SPENT_USD", 0.5, "llm");
  bb.post("NEW_SESSION", nlohmann::json::object(), "chat");
  bb.pump();
  auto st = status_of(bb);
  EXPECT_EQ(st.value("ctx_tokens", -1), 0);
  EXPECT_EQ(st.value("turn", -1), 0);
  EXPECT_DOUBLE_EQ(st.value("spent_usd", 0.0), 0.5);
  EXPECT_EQ(st.value("model", ""), "m1");
}

TEST(StatusModule, MalformedPayloadsDoNotCrash) {
  Blackboard bb;
  StatusModule s;
  s.on_attach(bb);
  bb.post("LLM_RESPONSE", "not an object", "x");
  bb.post("LLM_REQUEST", 42, "x");
  bb.post("BUDGET_SPENT_USD", "NaN-ish string", "x");
  bb.post("LLM_RESPONSE", {{"prompt_tokens", "str"}, {"completion_tokens", nullptr}}, "x");
  bb.pump();   // must not throw
  SUCCEED();
}

TEST(StatusModule, FormatStatusLine) {
  EXPECT_EQ(format_status(12437, 0.0372, 9, "gpt-5.5"),
            "[ctx 12.4k tok · $0.0372 · turn 9 · gpt-5.5]");
  EXPECT_EQ(format_status(999, 0.0, 1, ""), "[ctx 999 tok · $0.0000 · turn 1]");
  EXPECT_EQ(format_status(0, 0.0, 0, ""), "[ctx 0 tok · $0.0000 · turn 0]");
}
