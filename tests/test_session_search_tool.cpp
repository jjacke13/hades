// tests/test_session_search_tool.cpp — drive the hades-session-search binary over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
std::string fresh_dir(const char* tag) {
  const std::string d =
      ::testing::TempDir() + "/sess_search_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
void write_session(const std::string& dir, const std::string& name,
                   const std::vector<std::pair<std::string, std::string>>& turns) {
  std::ofstream f(dir + "/" + name);
  for (const auto& [u, a] : turns) {
    f << nlohmann::json{{"role", "user"}, {"content", u}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", a}}.dump() << "\n";
  }
}
nlohmann::json search(const std::vector<std::string>& argv_tail, const nlohmann::json& args) {
  std::vector<std::string> argv{SESSION_SEARCH_BIN};
  for (const auto& s : argv_tail) argv.push_back(s);
  nlohmann::json call{{"call", "session_search"}, {"args", args}};
  ProcResult r = run_subprocess(argv, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(SessionSearchTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SESSION_SEARCH_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "session_search");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "query") != required.end());
}

TEST(SessionSearchTool, RanksByTokenOverlapNewestFirst) {
  const std::string dir = fresh_dir("rank");
  write_session(dir, "20260701-100000.jsonl",
                {{"tell me about the pi zero deployment", "we deployed hades to the pi"}});
  write_session(dir, "20260710-100000.jsonl",
                {{"unrelated chat about weather", "sunny"},
                 {"pi zero deployment status?", "pi deployment is live"}});
  auto j = search({dir}, {{"query", "pi deployment"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  const auto& hits = j["result"]["hits"];
  ASSERT_GE(hits.size(), 2u);
  // Both matching turns found; the newest session's hit ranks first on equal overlap.
  EXPECT_EQ(hits[0].value("session", ""), "20260710-100000");
  EXPECT_NE(hits[0].value("text", "").find("pi deployment is live"), std::string::npos);
  EXPECT_EQ(j["result"].value("searched_sessions", 0), 2);
  // The weather turn (zero overlap) is not a hit.
  for (const auto& h : hits)
    EXPECT_EQ(h.value("text", "").find("sunny"), std::string::npos);
}

TEST(SessionSearchTool, LiveSessionExcludedByFilename) {
  const std::string dir = fresh_dir("live");
  write_session(dir, "old.jsonl", {{"magic keyword alpha", "noted"}});
  write_session(dir, "live.jsonl", {{"magic keyword alpha", "live copy"}});
  auto j = search({dir, "live.jsonl"}, {{"query", "magic keyword alpha"}});
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["hits"].size(), 1u);
  EXPECT_EQ(j["result"]["hits"][0].value("session", ""), "old");
  EXPECT_EQ(j["result"].value("searched_sessions", 0), 1);
}

TEST(SessionSearchTool, MaxResultsClampAndTruncation) {
  const std::string dir = fresh_dir("clamp");
  std::vector<std::pair<std::string, std::string>> turns;
  for (int i = 0; i < 30; ++i)
    turns.push_back({"needle number " + std::to_string(i), std::string(2000, 'x')});
  write_session(dir, "s.jsonl", turns);
  auto j = search({dir}, {{"query", "needle"}, {"max_results", 999}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"]["hits"].size(), 20u);                       // clamped to 20
  EXPECT_LE(j["result"]["hits"][0].value("text", "").size(), 700u); // unit truncated
  auto j2 = search({dir}, {{"query", "needle"}});
  EXPECT_EQ(j2["result"]["hits"].size(), 5u);                       // default 5
}

TEST(SessionSearchTool, EmptyQueryFailsClosed) {
  const std::string dir = fresh_dir("empty");
  for (const char* raw :
       {R"({"call":"session_search","args":{}})",
        R"({"call":"session_search","args":{"query":""}})",
        R"({"call":"session_search","args":{"query":42}})"}) {
    ProcResult r = run_subprocess({SESSION_SEARCH_BIN, dir}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}

TEST(SessionSearchTool, NoHitsAndMissingDirAreOkEmpty) {
  const std::string dir = fresh_dir("nohits");
  write_session(dir, "s.jsonl", {{"hello there", "hi"}});
  auto j = search({dir}, {{"query", "zzz_nomatch_zzz"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"]["hits"].empty());
  auto j2 = search({"/nonexistent/sessions/dir"}, {{"query", "anything"}});
  ASSERT_TRUE(j2.value("ok", false));
  EXPECT_TRUE(j2["result"]["hits"].empty());
  EXPECT_EQ(j2["result"].value("searched_sessions", -1), 0);
}

TEST(SessionSearchTool, CorruptLinesSkippedNonJsonlIgnored) {
  const std::string dir = fresh_dir("corrupt");
  {
    std::ofstream f(dir + "/c.jsonl");
    f << "{not json\n";
    f << nlohmann::json{{"role", "user"}, {"content", "findable token here"}}.dump() << "\n";
    f << nlohmann::json{{"role", "assistant"}, {"content", "answer"}}.dump() << "\n";
  }
  std::ofstream(dir + "/notes.txt") << "findable token here but wrong extension\n";
  auto j = search({dir}, {{"query", "findable token"}});
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["hits"].size(), 1u);
  EXPECT_EQ(j["result"]["hits"][0].value("session", ""), "c");
}
