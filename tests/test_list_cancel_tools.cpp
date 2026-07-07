// tests/test_list_cancel_tools.cpp — hades-list-tasks + hades-cancel-task over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string store_with(const char* tag, const std::string& contents) {
  const std::string p =
      (fs::path(::testing::TempDir()) / ("lc_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  std::ofstream f(p, std::ios::trunc); f << contents; return p;
}
static const char* kAdd =
    R"({"op":"add","id":"t1-aaaa","name":"nightly","kind":"cron","schedule":"0 3 * * *","fire_epoch":null,"prompt":"p","notify":true,"created":1})";

TEST(ListTasksTool, DescribeAndListActive) {
  const std::string store = store_with("list", std::string(kAdd) + "\n");
  ProcResult d = run_subprocess({LIST_TASKS_BIN}, R"({"call":"describe"})", 30.0);
  EXPECT_EQ(nlohmann::json::parse(d.out, nullptr, false)["result"].value("name", ""), "list_tasks");
  ProcResult r = run_subprocess({LIST_TASKS_BIN, store}, R"({"call":"list_tasks"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  auto tasks = j["result"]["tasks"];
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0].value("id", ""), "t1-aaaa");
  EXPECT_EQ(tasks[0].value("name", ""), "nightly");
}

TEST(ListTasksTool, EmptyOrMissingStoreYieldsEmpty) {
  ProcResult r = run_subprocess({LIST_TASKS_BIN, "/nonexistent/cron.jsonl"}, R"({"call":"list_tasks"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"]["tasks"].empty());
}

TEST(CancelTaskTool, CancelActiveAppendsTombstone) {
  const std::string store = store_with("cancel", std::string(kAdd) + "\n");
  ProcResult r = run_subprocess({CANCEL_TASK_BIN, store}, R"({"call":"cancel_task","args":{"id":"t1-aaaa"}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_TRUE(j["result"].value("cancelled", false));
  // re-list -> gone
  ProcResult r2 = run_subprocess({LIST_TASKS_BIN, store}, R"({"call":"list_tasks"})", 30.0);
  EXPECT_TRUE(nlohmann::json::parse(r2.out, nullptr, false)["result"]["tasks"].empty());
}

TEST(CancelTaskTool, CancelUnknownIdIsNotOk) {
  const std::string store = store_with("unknown", std::string(kAdd) + "\n");
  ProcResult r = run_subprocess({CANCEL_TASK_BIN, store}, R"({"call":"cancel_task","args":{"id":"ghost"}})", 30.0);
  EXPECT_FALSE(nlohmann::json::parse(r.out, nullptr, false).value("ok", true));
}
