// tests/test_self_scheduling_wiring.cpp — manifest wiring for self-scheduling (real tools via ToolRunner)
// No llm module (the skills-wiring precedent): we post TOOL_REQUEST straight to the ToolRunner, which
// runs the REAL binaries synchronously on pump. The resolved argv (store + caps) is proven by the
// SIDE EFFECTS on cron_store — there is no argv accessor, and inventing one is the wrong pattern.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"  // MalConfig
#include "hades/heartbeat/cron_store.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string wire_store(const char* tag) {
  const std::string p = (fs::path(::testing::TempDir()) /
                         ("sw_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
  fs::remove(p);
  return p;
}
// SCHEDULE_TASK_BIN / LIST_TASKS_BIN / CANCEL_TASK_BIN are the compiled target paths (compile-defs
// on hades_tests, added in Tasks 2-3), so the wired argv points at the real binaries.
static std::string manifest(const std::string& cron_store, const char* max_tasks = "20") {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = arbiter\n" +
         "Module = heartbeat\n" +
         "Tool = schedule_task { native = " + SCHEDULE_TASK_BIN + " }\n" +
         "Tool = list_tasks    { native = " + LIST_TASKS_BIN + " }\n" +
         "Tool = cancel_task   { native = " + CANCEL_TASK_BIN + " }\n" +
         "Heartbeat\n{\n  cron_store = " + cron_store + "\n  max_tasks = " + max_tasks +
         "\n  min_interval_s = 90\n}\n" +
         "Heartbeat = nightly\n{\n  schedule = 0 3 * * *\n  prompt = do it\n}\n";
}
static std::vector<CronTask> fold_file(const std::string& path) {
  std::ifstream f(path); std::stringstream s; s << f.rdbuf();
  return fold_cron_store(s.str());
}
static void post_schedule(Blackboard& bb, const char* id) {
  bb.post("TOOL_REQUEST",
          {{"id", id}, {"tool", "schedule_task"},
           {"args", {{"name", "n"}, {"prompt", "p"}, {"schedule", "* * * * *"}}}}, "arbiter");
  bb.pump();
}

TEST(SelfSchedulingWiring, ScheduleTaskWritesToWiredStore) {
  const std::string store = wire_store("store");
  Blackboard bb;
  Manifest m = parse_manifest(manifest(store));
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.tools, nullptr);
  post_schedule(bb, "c1");
  ASSERT_TRUE(fs::exists(store));           // argv carried the wired cron_store
  EXPECT_EQ(fold_file(store).size(), 1u);
}

TEST(SelfSchedulingWiring, MaxTasksCapReachesTheBinary) {
  const std::string store = wire_store("cap");
  Blackboard bb;
  Manifest m = parse_manifest(manifest(store, "1"));   // cap = 1
  Agent agent = build_agent(bb, m);
  post_schedule(bb, "a");   // ok
  post_schedule(bb, "b");   // cap 1 reached -> the binary refuses
  EXPECT_EQ(fold_file(store).size(), 1u);   // only the first add survived -> caps threaded via argv
}

TEST(SelfSchedulingWiring, UnnamedConfigBlockIsNotParsedAsEntry) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest(wire_store("cfg")));
  EXPECT_NO_THROW(build_agent(bb, m));   // the config block has no `schedule` -> must NOT MalConfig
}

TEST(SelfSchedulingWiring, WhitespaceCronStoreThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest("/tmp/has space.jsonl"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}
