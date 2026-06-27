// tests/test_tools.cpp — the bundled native tool binaries (shell/write_file/list_dir/http_fetch)
//
// Drives each built tool binary through hades::run_subprocess over the one-JSON-line
// protocol: every tool answers `describe` with its name, and the offline tools
// (shell/write_file/list_dir) are exercised functionally. http_fetch is describe-only
// here (a live GET would need the network). Binary paths come from CMake compile-defs.

#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

static nlohmann::json call_tool(const std::string& bin, const nlohmann::json& req) {
  ProcResult r = run_subprocess({bin}, req.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(Tools, AllToolsDescribeWithTheirName) {
  std::vector<std::pair<std::string, std::string>> tools = {
      {SHELL_BIN, "shell"},
      {WRITE_FILE_BIN, "write_file"},
      {LIST_DIR_BIN, "list_dir"},
      {HTTP_FETCH_BIN, "http_fetch"}};
  for (auto& [bin, name] : tools) {
    auto j = call_tool(bin, {{"call", "describe"}});
    ASSERT_TRUE(j.is_object() && j.value("ok", false)) << bin;
    EXPECT_EQ(j["result"].value("name", ""), name);
    EXPECT_TRUE(j["result"].contains("schema"));
  }
}

TEST(Tools, ShellRunsCommand) {
  auto j = call_tool(SHELL_BIN, {{"call", "shell"}, {"args", {{"cmd", "printf hello"}}}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("stdout", ""), "hello");
  EXPECT_EQ(j["result"].value("code", -1), 0);
}

TEST(Tools, WriteFileThenListDirSeesIt) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/tool_write.txt";
  auto w = call_tool(WRITE_FILE_BIN,
                     {{"call", "write_file"}, {"args", {{"path", path}, {"content", "DATA42"}}}});
  ASSERT_TRUE(w.value("ok", false));
  std::ifstream f(path);
  std::string got;
  std::getline(f, got);
  EXPECT_EQ(got, "DATA42");

  auto l = call_tool(LIST_DIR_BIN, {{"call", "list_dir"}, {"args", {{"path", dir}}}});
  ASSERT_TRUE(l.value("ok", false));
  bool found = false;
  for (auto& e : l["result"]["entries"])
    if (e.value("name", "") == "tool_write.txt") found = true;
  EXPECT_TRUE(found);
}

TEST(Tools, UnknownCallIsNotOk) {
  auto j = call_tool(SHELL_BIN, {{"call", "bogus"}});
  EXPECT_FALSE(j.value("ok", true));
}
