// tests/test_tools.cpp — the bundled native tool binaries (shell/write_file/list_dir/http_fetch)
//
// Drives each built tool binary through hades::run_subprocess over the one-JSON-line
// protocol: every tool answers `describe` with its name, and the offline tools
// (shell/write_file/list_dir) are exercised functionally. http_fetch is describe-only
// here (a live GET would need the network). Binary paths come from CMake compile-defs.

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/tool/file_version.h"
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

TEST(Tools, FsReadReportsContentVersion) {
  const std::string path = ::testing::TempDir() + "/ver_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "the content\n"; }
  auto j = call_tool(FS_READ_BIN, {{"call", "fs_read"}, {"args", {{"path", path}}}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("content", ""), "the content\n");
  EXPECT_EQ(j["result"].value("version", ""), hades::file_version("the content\n"));
}

TEST(Tools, WriteFileDescribeSchemaDoesNotMentionExpectVersion) {
  ProcResult r = run_subprocess({WRITE_FILE_BIN}, R"({"call":"describe"})", 30.0);
  EXPECT_EQ(r.out.find("expect_version"), std::string::npos);   // Arbiter plumbing, not LLM API
}

TEST(Tools, WriteFileExpectVersionGate) {
  const std::string path = ::testing::TempDir() + "/wv_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "original"; }
  // Mismatch -> refused, untouched.
  auto bad = call_tool(WRITE_FILE_BIN,
      {{"call", "write_file"},
       {"args", {{"path", path}, {"content", "clobber"}, {"expect_version", "0000000000000000"}}}});
  EXPECT_FALSE(bad.value("ok", true));
  EXPECT_NE(bad["result"].value("error", "").find("changed on disk"), std::string::npos);
  { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); EXPECT_EQ(s.str(), "original"); }
  // Match -> written, version of the NEW content reported.
  auto ok = call_tool(WRITE_FILE_BIN,
      {{"call", "write_file"},
       {"args",
        {{"path", path}, {"content", "fresh"}, {"expect_version", hades::file_version("original")}}}});
  ASSERT_TRUE(ok.value("ok", false)) << ok.dump();
  EXPECT_EQ(ok["result"].value("version", ""), hades::file_version("fresh"));
  { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); EXPECT_EQ(s.str(), "fresh"); }
  // Cross-tool round-trip: a subsequent fs_read (text-mode) must report the SAME version the
  // binary-mode write stamped — the pair the guard's map consistency rides on.
  auto rd = call_tool(FS_READ_BIN, {{"call", "fs_read"}, {"args", {{"path", path}}}});
  ASSERT_TRUE(rd.value("ok", false));
  EXPECT_EQ(rd["result"].value("version", ""), ok["result"].value("version", ""));
}

TEST(Tools, WriteFileExpectVersionOnDeletedFileRefuses) {
  const std::string path = ::testing::TempDir() + "/wv_gone_" + std::to_string(::getpid()) + ".txt";
  std::filesystem::remove(path);  // file was read once, then deleted externally
  auto j = call_tool(WRITE_FILE_BIN,
      {{"call", "write_file"},
       {"args", {{"path", path}, {"content", "x"}, {"expect_version", "cbf29ce484222325"}}}});
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(std::filesystem::exists(path));  // nothing created
}

TEST(Tools, WriteFilePreservesModeOnOverwrite) {
  const std::string path = ::testing::TempDir() + "/wv_mode_" + std::to_string(::getpid()) + ".sh";
  { std::ofstream f(path, std::ios::trunc); f << "#!/bin/sh\n"; }
  ::chmod(path.c_str(), 0755);
  auto j = call_tool(WRITE_FILE_BIN,
      {{"call", "write_file"}, {"args", {{"path", path}, {"content", "#!/bin/sh\necho hi\n"}}}});
  ASSERT_TRUE(j.value("ok", false));
  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0755));  // exec bit survives the atomic rename
}
