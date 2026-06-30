#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "hades/embedding/session_turns.h"
using namespace hades;

static std::string write_sess(const std::string& name, const std::string& body) {
  std::string p = testing::TempDir() + "/" + name;
  std::ofstream f(p, std::ios::trunc); f << body; return p;
}

TEST(SessionTurns, PairsUserWithFollowingAssistant) {
  std::string p = write_sess("st1.jsonl",
    "{\"role\":\"user\",\"content\":\"q1\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a1\"}\n"
    "{\"role\":\"user\",\"content\":\"q2\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a2\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 2u);
  EXPECT_NE(turns[0].text.find("U: q1"), std::string::npos);
  EXPECT_NE(turns[0].text.find("A: a1"), std::string::npos);
  EXPECT_NE(turns[0].id.find("#0"), std::string::npos);
  EXPECT_NE(turns[1].id.find("#1"), std::string::npos);
}
TEST(SessionTurns, FoldsToolTurnsAndSkipsToolCallAssistant) {
  std::string p = write_sess("st2.jsonl",
    "{\"role\":\"user\",\"content\":\"read X\"}\n"
    "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\"}]}\n"
    "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"FILE\"}\n"
    "{\"role\":\"assistant\",\"content\":\"it says FILE\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 1u);                  // the user pairs with the FINAL text assistant
  EXPECT_NE(turns[0].text.find("U: read X"), std::string::npos);
  EXPECT_NE(turns[0].text.find("A: it says FILE"), std::string::npos);
}
TEST(SessionTurns, TrailingUserWithoutAssistantIsDropped) {
  std::string p = write_sess("st3.jsonl",
    "{\"role\":\"user\",\"content\":\"q1\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a1\"}\n"
    "{\"role\":\"user\",\"content\":\"dangling\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 1u);                  // dangling user (no answer yet) dropped
}
