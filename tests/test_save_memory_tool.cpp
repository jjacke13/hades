// tests/test_save_memory_tool.cpp — drive the hades-save-memory binary over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

TEST(SaveMemoryTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_memory");
  EXPECT_TRUE(j["result"].contains("schema"));
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "text") != required.end());
}

TEST(SaveMemoryTool, AppendsRecordLine) {
  const std::string store = ::testing::TempDir() + "/save_tool.jsonl";
  std::remove(store.c_str());
  nlohmann::json call{{"call", "save_memory"}, {"args", {{"text", "remember this"}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  std::ifstream f(store);
  std::string line;
  std::getline(f, line);
  auto rec = nlohmann::json::parse(line, nullptr, false);
  EXPECT_EQ(rec.value("text", ""), "remember this");
  EXPECT_TRUE(rec.contains("ts"));
  EXPECT_TRUE(rec["ts"].is_number());
}

TEST(SaveMemoryTool, MissingTextIsNotOk) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, ::testing::TempDir() + "/x.jsonl"},
                                R"({"call":"save_memory","args":{}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveMemoryTool, NonStringTextIsNotOkAndDoesNotCrash) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, ::testing::TempDir() + "/ns.jsonl"},
                                R"({"call":"save_memory","args":{"text":123}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());          // tool produced clean JSON, did not abort
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveMemoryTool, AppendDoesNotTruncate) {
  const std::string store = ::testing::TempDir() + "/append_check.jsonl";
  std::remove(store.c_str());
  auto call = [&](const std::string& text) {
    nlohmann::json c{{"call", "save_memory"}, {"args", {{"text", text}}}};
    run_subprocess({SAVE_MEMORY_BIN, store}, c.dump(), 30.0);
  };
  call("first");
  call("second");
  std::ifstream f(store);
  std::string l1, l2, l3;
  std::getline(f, l1);
  std::getline(f, l2);
  std::getline(f, l3);
  EXPECT_FALSE(l1.empty());   // first record present
  EXPECT_FALSE(l2.empty());   // second record present — not overwritten
  EXPECT_TRUE(l3.empty());    // exactly 2 lines
}

TEST(SaveMemoryTool, WritesTopicWhenGiven) {
  const std::string store = ::testing::TempDir() + "/save_topic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  nlohmann::json call{{"call", "save_memory"},
                      {"args", {{"text", "prefers window seats"}, {"topic", "seat-pref"}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false)) << r.out;
  std::ifstream f(store);
  std::string line;
  ASSERT_TRUE(std::getline(f, line));
  auto rec = nlohmann::json::parse(line, nullptr, false);
  ASSERT_TRUE(rec.is_object());
  EXPECT_EQ(rec.value("text", ""), "prefers window seats");
  EXPECT_EQ(rec.value("topic", ""), "seat-pref");
}

TEST(SaveMemoryTool, OmitsTopicWhenAbsentOrEmpty) {
  const std::string store = ::testing::TempDir() + "/save_notopic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  for (const auto& args : {nlohmann::json{{"text", "standalone note"}},
                           nlohmann::json{{"text", "standalone note"}, {"topic", ""}}}) {
    nlohmann::json call{{"call", "save_memory"}, {"args", args}};
    ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_TRUE(j.value("ok", false)) << r.out;
  }
  std::ifstream f(store);
  std::string line;
  while (std::getline(f, line)) {
    auto rec = nlohmann::json::parse(line, nullptr, false);
    ASSERT_TRUE(rec.is_object());
    EXPECT_FALSE(rec.contains("topic"));   // legacy-identical line shape
  }
}

TEST(SaveMemoryTool, NonStringTopicFailsClosed) {
  const std::string store = ::testing::TempDir() + "/save_badtopic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  nlohmann::json call{{"call", "save_memory"},
                      {"args", {{"text", "x"}, {"topic", 42}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));                    // house rule: non-string fails closed
  EXPECT_FALSE(std::filesystem::exists(store));         // and nothing was written
}

TEST(SaveMemoryTool, DescribeAdvertisesOptionalTopic) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  const auto& schema = j["result"]["schema"];
  EXPECT_TRUE(schema["properties"].contains("topic"));
  const auto req = schema.value("required", nlohmann::json::array());
  EXPECT_EQ(std::find(req.begin(), req.end(), "topic"), req.end());   // optional
  EXPECT_NE(std::find(req.begin(), req.end(), "text"), req.end());    // text still required
}
