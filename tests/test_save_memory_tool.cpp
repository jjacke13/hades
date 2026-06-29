// tests/test_save_memory_tool.cpp — drive the hades-save-memory binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

TEST(SaveMemoryTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_memory");
  EXPECT_TRUE(j["result"].contains("schema"));
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
}

TEST(SaveMemoryTool, MissingTextIsNotOk) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, ::testing::TempDir() + "/x.jsonl"},
                                R"({"call":"save_memory","args":{}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  EXPECT_FALSE(j.value("ok", true));
}
