// tests/test_schedule_task_tool.cpp — drive hades-schedule-task over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_store(const char* tag) {
  const std::string p =
      (fs::path(::testing::TempDir()) / ("sched_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  fs::remove(p);
  return p;
}
static nlohmann::json call_sched(const std::string& store, const std::string& raw,
                                 const char* maxt = "20", const char* mini = "60") {
  ProcResult r = run_subprocess({SCHEDULE_TASK_BIN, store, maxt, mini}, raw, 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str();
}

TEST(ScheduleTaskTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SCHEDULE_TASK_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "schedule_task");
  auto req = j["result"]["schema"].value("required", nlohmann::json::array());
  for (const char* k : {"name", "prompt"})
    EXPECT_TRUE(std::find(req.begin(), req.end(), k) != req.end()) << k;
}

TEST(ScheduleTaskTool, CronTaskAppendsAddRecord) {
  const std::string store = fresh_store("cron");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"nightly","prompt":"summarize","notify":true,"schedule":"0 3 * * *"}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "cron");
  EXPECT_FALSE(j["result"].value("id", "").empty());
  const std::string body = slurp(store);
  EXPECT_NE(body.find("\"op\":\"add\""), std::string::npos);
  EXPECT_NE(body.find("0 3 * * *"), std::string::npos);
}

TEST(ScheduleTaskTool, InvalidCronRejected) {
  const std::string store = fresh_store("badcron");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"x","prompt":"p","schedule":"not a cron"}})");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(fs::exists(store));   // nothing written on rejection
}

TEST(ScheduleTaskTool, InMinutesBecomesOnce) {
  const std::string store = fresh_store("in");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"ping","in_minutes":10}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "once");
}

TEST(ScheduleTaskTool, InMinutesBelowFloorRejected) {
  const std::string store = fresh_store("floor");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"p","in_minutes":0}})", "20", "120");
  EXPECT_FALSE(j.value("ok", true));   // 0*60 < 120s floor
}

TEST(ScheduleTaskTool, AtAbsoluteBecomesOnce) {
  const std::string store = fresh_store("at");
  auto j = call_sched(store,
      R"({"call":"schedule_task","args":{"name":"r","prompt":"p","at":"2030-01-01T09:00"}})");
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("kind", ""), "once");
}

TEST(ScheduleTaskTool, ExactlyOneTimingRequired) {
  const std::string store = fresh_store("timing");
  // none
  EXPECT_FALSE(call_sched(store, R"({"call":"schedule_task","args":{"name":"x","prompt":"p"}})").value("ok", true));
  // two
  EXPECT_FALSE(call_sched(store,
      R"({"call":"schedule_task","args":{"name":"x","prompt":"p","in_minutes":5,"schedule":"* * * * *"}})").value("ok", true));
}

TEST(ScheduleTaskTool, MaxTasksCapRefuses) {
  const std::string store = fresh_store("cap");
  ASSERT_TRUE(call_sched(store, R"({"call":"schedule_task","args":{"name":"a","prompt":"p","schedule":"* * * * *"}})", "1", "60").value("ok", false));
  auto j = call_sched(store, R"({"call":"schedule_task","args":{"name":"b","prompt":"p","schedule":"* * * * *"}})", "1", "60");
  EXPECT_FALSE(j.value("ok", true));   // active count 1 >= cap 1
}

TEST(ScheduleTaskTool, MissingArgsAndNonStringFailClosed) {
  const std::string store = fresh_store("bad");
  for (const char* raw : {
       R"({"call":"schedule_task","args":{"prompt":"p","schedule":"* * * * *"}})",     // no name
       R"({"call":"schedule_task","args":{"name":"x","schedule":"* * * * *"}})",       // no prompt
       R"({"call":"schedule_task","args":{"name":7,"prompt":"p","schedule":"* * * * *"}})"}) {
    ProcResult r = run_subprocess({SCHEDULE_TASK_BIN, store}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
