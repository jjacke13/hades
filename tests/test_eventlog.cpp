#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "hades/eventlog.h"
using namespace hades;
static std::string slurp(const std::string& p){ std::ifstream f(p); std::stringstream s; s<<f.rdbuf(); return s.str(); }

TEST(Eventlog, AppendsTsvLineAndKeepsMemory) {
  std::string p = testing::TempDir()+"/ev1.log";
  Eventlog ev(p);
  ev.append({"USER_MESSAGE", nlohmann::json("hi"), "chat", "", 0.5, 1});
  ASSERT_EQ(ev.entries().size(), 1u);
  std::string body = slurp(p);
  EXPECT_NE(body.find("USER_MESSAGE"), std::string::npos);
  EXPECT_NE(body.find("chat"), std::string::npos);
  EXPECT_NE(body.find('\t'), std::string::npos);
}
TEST(Eventlog, RedactsSecret) {
  std::string p = testing::TempDir()+"/ev2.log";
  Eventlog ev(p);
  ev.add_redaction("sk-TOPSECRET");
  ev.append({"LLM_REQUEST", nlohmann::json("key=sk-TOPSECRET"), "llm", "", 0.1, 2});
  std::string body = slurp(p);
  EXPECT_EQ(body.find("sk-TOPSECRET"), std::string::npos);
  EXPECT_NE(body.find("***REDACTED***"), std::string::npos);
}
TEST(Eventlog, RedactsSecretInMemory) {
  Eventlog ev("");                 // in-memory only
  ev.add_redaction("sk-TOPSECRET");
  ev.append({"LLM_REQUEST", nlohmann::json("auth: sk-TOPSECRET"), "llm", "", 0.1, 3});
  std::string dumped = ev.entries().back().value.dump();
  EXPECT_EQ(dumped.find("sk-TOPSECRET"), std::string::npos);
  EXPECT_NE(dumped.find("***REDACTED***"), std::string::npos);
}
TEST(Eventlog, RedactsSecretInKeyAndSource) {
  Eventlog ev("");
  ev.add_redaction("sekret");
  ev.append({"K_sekret", nlohmann::json("x"), "src_sekret", "aux_sekret", 0.1, 4});
  auto& back = ev.entries().back();
  EXPECT_EQ(back.key.find("sekret"), std::string::npos);
  EXPECT_EQ(back.source.find("sekret"), std::string::npos);
  EXPECT_EQ(back.aux.find("sekret"), std::string::npos);
}
