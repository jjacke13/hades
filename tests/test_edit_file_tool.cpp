// tests/test_edit_file_tool.cpp — drive hades-edit-file over the native protocol
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
std::string mk_file(const char* tag, const std::string& content) {
  const std::string p = ::testing::TempDir() + "/edit_" + tag + "_" + std::to_string(::getpid()) + ".txt";
  std::ofstream(p) << content;
  return p;
}
std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
nlohmann::json edit(const nlohmann::json& args) {
  ProcResult r = run_subprocess({EDIT_FILE_BIN}, nlohmann::json{{"call", "edit_file"}, {"args", args}}.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(EditFileTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({EDIT_FILE_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "edit_file");
}

TEST(EditFileTool, SingleUniqueReplace) {
  const std::string p = mk_file("one", "alpha beta gamma\n");
  auto j = edit({{"path", p}, {"old_string", "beta"}, {"new_string", "BETA"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("replacements", 0), 1);
  EXPECT_EQ(slurp(p), "alpha BETA gamma\n");
}

TEST(EditFileTool, AmbiguousWithoutReplaceAllFails) {
  const std::string p = mk_file("two", "x x x\n");
  auto j = edit({{"path", p}, {"old_string", "x"}, {"new_string", "y"}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("3"), std::string::npos);  // names the count
  EXPECT_EQ(slurp(p), "x x x\n");                                          // untouched
}

TEST(EditFileTool, ReplaceAll) {
  const std::string p = mk_file("all", "x x x\n");
  auto j = edit({{"path", p}, {"old_string", "x"}, {"new_string", "y"}, {"replace_all", true}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("replacements", 0), 3);
  EXPECT_EQ(slurp(p), "y y y\n");
}

TEST(EditFileTool, PreservesFileMode) {
  const std::string p = mk_file("mode", "alpha beta gamma\n");
  ASSERT_EQ(::chmod(p.c_str(), 0750), 0);
  auto j = edit({{"path", p}, {"old_string", "beta"}, {"new_string", "BETA"}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  struct stat st{};
  ASSERT_EQ(::stat(p.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0750u);
}

TEST(EditFileTool, FailClosedPaths) {
  const std::string p = mk_file("fc", "content\n");
  EXPECT_FALSE(edit({{"path", p}, {"old_string", "absent"}, {"new_string", "n"}}).value("ok", true));
  EXPECT_FALSE(edit({{"path", p}, {"old_string", ""}, {"new_string", "n"}}).value("ok", true));
  EXPECT_FALSE(edit({{"path", p}, {"old_string", "same"}, {"new_string", "same"}}).value("ok", true));
  EXPECT_FALSE(edit({{"path", "/nope/nope.txt"}, {"old_string", "a"}, {"new_string", "b"}}).value("ok", true));
  EXPECT_FALSE(edit({{"path", p}, {"old_string", 7}, {"new_string", "b"}}).value("ok", true));  // typed
  EXPECT_EQ(slurp(p), "content\n");
}
