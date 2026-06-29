// tests/test_pin_fact_tool.cpp — drive the hades-pin-fact binary over the native protocol
#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

TEST(PinFactTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({PIN_FACT_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "pin_fact");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "text") != required.end());
}

TEST(PinFactTool, AppendsBulletLineAndCreatesDir) {
  const std::string dir = ::testing::TempDir() + "/pf_dir_" + std::to_string(::getpid());
  std::filesystem::remove_all(dir);
  const std::string file = dir + "/facts.md";   // parent dir does not exist yet
  nlohmann::json call{{"call", "pin_fact"}, {"args", {{"text", "user is based in Greece"}}}};
  ProcResult r = run_subprocess({PIN_FACT_BIN, file}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  std::ifstream f(file);
  std::string line;
  std::getline(f, line);
  EXPECT_EQ(line, "- user is based in Greece");
}

TEST(PinFactTool, AppendDoesNotTruncate) {
  const std::string file = ::testing::TempDir() + "/pf_append.md";
  std::remove(file.c_str());
  auto pin = [&](const std::string& t) {
    nlohmann::json c{{"call", "pin_fact"}, {"args", {{"text", t}}}};
    run_subprocess({PIN_FACT_BIN, file}, c.dump(), 30.0);
  };
  pin("first");
  pin("second");
  std::ifstream f(file);
  std::string l1, l2, l3;
  std::getline(f, l1);
  std::getline(f, l2);
  std::getline(f, l3);
  EXPECT_EQ(l1, "- first");
  EXPECT_EQ(l2, "- second");
  EXPECT_TRUE(l3.empty());
}

TEST(PinFactTool, NonStringTextIsNotOkAndDoesNotCrash) {
  ProcResult r = run_subprocess({PIN_FACT_BIN, ::testing::TempDir() + "/pf_ns.md"},
                                R"({"call":"pin_fact","args":{"text":123}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());        // produced clean JSON, did not abort
  EXPECT_FALSE(j.value("ok", true));
}

TEST(PinFactTool, NonStringCallIsNotOk) {
  ProcResult r = run_subprocess({PIN_FACT_BIN}, R"({"call":42})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());     // produced clean JSON, did not abort
  EXPECT_FALSE(j.value("ok", true));
}
