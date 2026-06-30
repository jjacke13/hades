#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "hades/session_history.h"
using namespace hades;

static std::string write_tmp(const std::string& name, const std::string& body) {
  std::string p = testing::TempDir() + "/" + name;
  std::ofstream f(p);
  f << body;
  return p;
}

TEST(SessionHistory, RoundTripsMessages) {
  std::string p = write_tmp("sh_ok.jsonl",
    "{\"role\":\"user\",\"content\":\"hi\"}\n"
    "{\"role\":\"assistant\",\"content\":\"hello\"}\n");
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].value("role", ""), "user");
  EXPECT_EQ(v[0].value("content", ""), "hi");
  EXPECT_EQ(v[1].value("content", ""), "hello");
}

TEST(SessionHistory, MissingFileAndEmptyPathYieldEmpty) {
  EXPECT_TRUE(read_session_jsonl("").empty());
  EXPECT_TRUE(read_session_jsonl(testing::TempDir() + "/sh_does_not_exist.jsonl").empty());
}

TEST(SessionHistory, SkipsBlankCorruptAndPartialTrailing) {
  std::string p = write_tmp("sh_dirty.jsonl",
    "{\"role\":\"user\",\"content\":\"a\"}\n"
    "\n"                                        // blank line
    "not json at all\n"                         // corrupt interior
    "[1,2,3]\n"                                 // valid JSON but not an object
    "{\"role\":\"assistant\",\"content\":\"b\"}\n"
    "{\"role\":\"user\",\"content\":\"trunc");  // partial trailing (unterminated, no newline)
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].value("content", ""), "a");
  EXPECT_EQ(v[1].value("content", ""), "b");
}

TEST(SessionHistory, KeepsTrailingToolCallOrphan) {
  // Display reader keeps everything parseable (unlike Arbiter::load_history, which strips
  // boundary orphans for provider validity): a dangling assistant(tool_calls) survives.
  std::string p = write_tmp("sh_orphan.jsonl",
    "{\"role\":\"user\",\"content\":\"go\"}\n"
    "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"
    "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"fs_read\",\"arguments\":\"{}\"}}]}\n");
  auto v = read_session_jsonl(p);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_TRUE(v[1].contains("tool_calls"));
}
