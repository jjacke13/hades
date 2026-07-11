// tests/test_core_memory_tool.cpp — drive the hades-core-memory binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_file(const char* tag) {
  const std::string f =
      ::testing::TempDir() + "/cm_" + tag + "_" + std::to_string(::getpid()) + ".md";
  std::remove(f.c_str());
  return f;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  return std::string((std::istreambuf_iterator<char>(f)), {});
}
static nlohmann::json call_tool(const std::vector<std::string>& argv, const nlohmann::json& args) {
  nlohmann::json c{{"call", "core_memory"}, {"args", args}};
  ProcResult r = run_subprocess(argv, c.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(CoreMemoryTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({CORE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "core_memory");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "action") != required.end());
}

TEST(CoreMemoryTool, AddAppendsBulletCreatesDirAndReportsUsage) {
  const std::string dir = ::testing::TempDir() + "/cm_dir_" + std::to_string(::getpid());
  fs::remove_all(dir);
  const std::string file = dir + "/facts.md";   // parent dir does not exist yet
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "user is based in Greece"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- user is based in Greece\n");
  EXPECT_EQ(j["result"].value("action", ""), "add");
  EXPECT_EQ(j["result"].value("entries", 0), 1);
  EXPECT_EQ(j["result"].value("cap", 0), 2400);           // default cap reported
  EXPECT_GT(j["result"].value("chars", 0), 0);
}

TEST(CoreMemoryTool, AddDuplicateRejected) {
  const std::string file = fresh_file("dup");
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "same"}}).value("ok", false));
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "same"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("already pinned"), std::string::npos);
  EXPECT_EQ(slurp(file), "- same\n");                     // still exactly one line
}

TEST(CoreMemoryTool, AddOverflowListsEntriesAndWritesNothing) {
  const std::string file = fresh_file("cap");
  // cap 30: first short add fits, second would exceed -> the consolidation error.
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file, "30"}, {{"action", "add"}, {"text", "first fact"}})
                  .value("ok", false));
  const std::string before = slurp(file);
  auto j = call_tool({CORE_MEMORY_BIN, file, "30"},
                     {{"action", "add"}, {"text", "second fact that is far too long for the cap"}});
  EXPECT_FALSE(j.value("ok", true));
  const std::string e = j["result"].value("error", "");
  EXPECT_NE(e.find("core memory full"), std::string::npos);
  EXPECT_NE(e.find("/30 chars"), std::string::npos);      // usage vs cap shown
  EXPECT_NE(e.find("1. - first fact"), std::string::npos);// numbered entry list
  EXPECT_NE(e.find("replace/remove"), std::string::npos); // consolidation instruction
  EXPECT_EQ(slurp(file), before);                          // nothing written
}

TEST(CoreMemoryTool, ReplaceSingleMatchRewritesLine) {
  const std::string file = fresh_file("rep");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "user lives in Athens"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "likes coffee"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "Athens"}, {"text", "user lives in Patras"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- user lives in Patras\n- likes coffee\n");
}

TEST(CoreMemoryTool, ReplaceNoMatchFails) {
  const std::string file = fresh_file("rep0");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "something"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "ghost"}, {"text", "x"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("no entry matches"), std::string::npos);
}

TEST(CoreMemoryTool, ReplaceAmbiguousFailsListingMatches) {
  const std::string file = fresh_file("repN");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "coffee in the morning"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "coffee after lunch"}});
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "coffee"}, {"text", "tea"}});
  EXPECT_FALSE(j.value("ok", true));
  const std::string e = j["result"].value("error", "");
  EXPECT_NE(e.find("ambiguous"), std::string::npos);
  EXPECT_NE(e.find("coffee in the morning"), std::string::npos);   // both candidates listed
  EXPECT_NE(e.find("coffee after lunch"), std::string::npos);
  EXPECT_EQ(slurp(file), "- coffee in the morning\n- coffee after lunch\n");   // untouched
}

TEST(CoreMemoryTool, ReplaceOverflowFailsAndWritesNothing) {
  const std::string file = fresh_file("repcap");
  ASSERT_TRUE(call_tool({CORE_MEMORY_BIN, file, "30"}, {{"action", "add"}, {"text", "short"}})
                  .value("ok", false));
  const std::string before = slurp(file);
  auto j = call_tool({CORE_MEMORY_BIN, file, "30"},
                     {{"action", "replace"}, {"match", "short"},
                      {"text", "a replacement far too long for the tiny cap"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("core memory full"), std::string::npos);
  EXPECT_EQ(slurp(file), before);
}

TEST(CoreMemoryTool, RemoveSingleMatchDeletesLine) {
  const std::string file = fresh_file("rm");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "keep me"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "drop me"}});
  auto j = call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "drop"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- keep me\n");
  EXPECT_EQ(j["result"].value("entries", -1), 1);
}

TEST(CoreMemoryTool, RemoveNoMatchAndAmbiguousFailClosed) {
  const std::string file = fresh_file("rmN");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "alpha one"}});
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "alpha two"}});
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "ghost"}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "remove"}, {"match", "alpha"}})
                   .value("ok", true));
  EXPECT_EQ(slurp(file), "- alpha one\n- alpha two\n");   // both survive both failures
}

TEST(CoreMemoryTool, HandEditedNonBulletLinesAreEntries) {
  const std::string file = fresh_file("hand");
  { std::ofstream f(file); f << "goal: world domination\n"; }   // user-added, no bullet
  auto j = call_tool({CORE_MEMORY_BIN, file},
                     {{"action", "replace"}, {"match", "domination"}, {"text", "goal: be helpful"}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(slurp(file), "- goal: be helpful\n");          // canonicalized to a bullet
}

TEST(CoreMemoryTool, EmptyStringArgsAreAbsent) {
  const std::string file = fresh_file("empty");
  // Empty text on add / empty match on remove: missing-arg errors, not weird matches.
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", ""}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file},
                         {{"action", "remove"}, {"match", ""}, {"text", ""}})
                   .value("ok", true));
  EXPECT_FALSE(call_tool({CORE_MEMORY_BIN, file}, {{"action", ""}, {"text", "x"}})
                   .value("ok", true));
}

TEST(CoreMemoryTool, UnknownActionFails) {
  auto j = call_tool({CORE_MEMORY_BIN, fresh_file("act")}, {{"action", "append"}, {"text", "x"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("add, replace, remove"), std::string::npos);
}

TEST(CoreMemoryTool, StripsEmbeddedNewlinesFromText) {
  const std::string file = fresh_file("nl");
  call_tool({CORE_MEMORY_BIN, file}, {{"action", "add"}, {"text", "real fact\n## Injected heading"}});
  std::ifstream f(file);
  std::string l1, l2;
  std::getline(f, l1);
  std::getline(f, l2);
  EXPECT_NE(l1.find("real fact"), std::string::npos);
  EXPECT_NE(l1.find("Injected"), std::string::npos);   // folded onto the SAME line
  EXPECT_TRUE(l2.empty());                              // no second line -> no injected structure
}

TEST(CoreMemoryTool, GarbageCapArgvFallsBackToDefault) {
  const std::string file = fresh_file("gcap");
  // "banana" and "-5" both -> default 2400; a normal add must succeed, and report cap 2400.
  auto j1 = call_tool({CORE_MEMORY_BIN, file, "banana"}, {{"action", "add"}, {"text", "a fact"}});
  ASSERT_TRUE(j1.value("ok", false));
  EXPECT_EQ(j1["result"].value("cap", 0), 2400);
  auto j2 = call_tool({CORE_MEMORY_BIN, file, "-5"}, {{"action", "add"}, {"text", "another"}});
  ASSERT_TRUE(j2.value("ok", false));
  EXPECT_EQ(j2["result"].value("cap", 0), 2400);
  // Overlong digits overflow strtoll (ERANGE -> LLONG_MAX): must fall back, not run uncapped.
  auto j3 = call_tool({CORE_MEMORY_BIN, file, "99999999999999999999999999"},
                      {{"action", "add"}, {"text", "third"}});
  ASSERT_TRUE(j3.value("ok", false));
  EXPECT_EQ(j3["result"].value("cap", 0), 2400);
}

TEST(CoreMemoryTool, NonStringArgsAndCallFailClosed) {
  ProcResult r1 = run_subprocess({CORE_MEMORY_BIN, fresh_file("ns")},
                                 R"({"call":"core_memory","args":{"action":"add","text":123}})", 30.0);
  auto j1 = nlohmann::json::parse(r1.out, nullptr, false);
  ASSERT_FALSE(j1.is_discarded());
  EXPECT_FALSE(j1.value("ok", true));
  ProcResult r2 = run_subprocess({CORE_MEMORY_BIN}, R"({"call":42})", 30.0);
  auto j2 = nlohmann::json::parse(r2.out, nullptr, false);
  ASSERT_FALSE(j2.is_discarded());
  EXPECT_FALSE(j2.value("ok", true));
}
