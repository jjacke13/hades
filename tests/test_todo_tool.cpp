// tests/test_todo_tool.cpp — drive the hades-todo binary over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_file(const char* tag) {
  const std::string f =
      ::testing::TempDir() + "/todo_" + tag + "_" + std::to_string(::getpid()) + ".md";
  fs::remove(f);
  return f;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
static nlohmann::json call_todo(const std::string& file, const nlohmann::json& args) {
  nlohmann::json req{{"call", "todo"}, {"args", args}};
  ProcResult r = run_subprocess({TODO_BIN, file}, req.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(TodoTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({TODO_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "todo");
  EXPECT_TRUE(j["result"]["schema"]["properties"].contains("items"));
  // the description must tell the model this REPLACES the whole list
  EXPECT_NE(j["result"].value("description", "").find("REPLACE"), std::string::npos);
}

TEST(TodoTool, ReplacesWholeListWithStatuses) {
  const std::string f = fresh_file("replace");
  auto j = call_todo(f, {{"items",
                          {{{"text", "step one"}},
                           {{"text", "step two"}, {"status", "in_progress"}},
                           {{"text", "step zero"}, {"status", "done"}}}}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("items", -1), 3);
  EXPECT_EQ(slurp(f), "- [ ] step one\n- [~] step two\n- [x] step zero\n");
  ASSERT_TRUE(call_todo(f, {{"items", {{{"text", "only"}}}}}).value("ok", false));
  EXPECT_EQ(slurp(f), "- [ ] only\n");   // second call REPLACED, not appended
}

TEST(TodoTool, EmptyArrayClearsList) {
  const std::string f = fresh_file("clear");
  ASSERT_TRUE(call_todo(f, {{"items", {{{"text", "x"}}}}}).value("ok", false));
  ASSERT_TRUE(call_todo(f, {{"items", nlohmann::json::array()}}).value("ok", false));
  EXPECT_EQ(slurp(f), "");
}

TEST(TodoTool, CapsRefuseWholeCall) {
  const std::string f = fresh_file("caps");
  nlohmann::json many = nlohmann::json::array();
  for (int i = 0; i < 21; ++i) many.push_back({{"text", "t" + std::to_string(i)}});
  EXPECT_FALSE(call_todo(f, {{"items", many}}).value("ok", true));
  EXPECT_FALSE(fs::exists(f));                       // nothing written on refusal
  EXPECT_FALSE(call_todo(f, {{"items", {{{"text", std::string(201, 'a')}}}}}).value("ok", true));
  EXPECT_FALSE(fs::exists(f));
}

TEST(TodoTool, UnknownStatusRefusesWholeCall) {
  const std::string f = fresh_file("status");
  auto j = call_todo(f, {{"items", {{{"text", "good"}},
                                    {{"text", "bad"}, {"status", "donee"}}}}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("pending | in_progress | done"),
            std::string::npos);
  EXPECT_FALSE(fs::exists(f));                       // whole-call refusal, no partial write
}

TEST(TodoTool, NewlinesFoldAndEmptyStatusIsPending) {
  const std::string f = fresh_file("fold");
  ASSERT_TRUE(call_todo(f, {{"items", {{{"text", "a\nb"}, {"status", ""}}}}}).value("ok", false));
  EXPECT_EQ(slurp(f), "- [ ] a b\n");
}

TEST(TodoTool, MalformedInputsFailClosed) {
  const std::string f = fresh_file("malformed");
  EXPECT_FALSE(call_todo(f, nlohmann::json::object()).value("ok", true));        // no items
  EXPECT_FALSE(call_todo(f, {{"items", "notanarray"}}).value("ok", true));
  EXPECT_FALSE(call_todo(f, {{"items", {"bare string"}}}).value("ok", true));    // item not object
  EXPECT_FALSE(call_todo(f, {{"items", {{{"text", 42}}}}}).value("ok", true));   // non-string text
  EXPECT_FALSE(call_todo(f, {{"items", {{{"text", ""}}}}}).value("ok", true));   // empty text
  EXPECT_FALSE(fs::exists(f));
  ProcResult r = run_subprocess({TODO_BIN, f}, "not json", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}
