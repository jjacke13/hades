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
