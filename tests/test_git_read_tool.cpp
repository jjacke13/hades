// tests/test_git_read_tool.cpp — drive hades-git-read against a scratch git repo
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
// Build a scratch repo: one committed file + one unstaged modification.
std::string mk_repo(const char* tag) {
  const std::string root = ::testing::TempDir() + "/gitread_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  auto git = [&](const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"git", "-C", root};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = run_subprocess(argv, "", 30.0);
    ASSERT_EQ(r.code, 0) << r.err;
  };
  git({"init", "-q"});
  git({"config", "user.email", "t@t"});
  git({"config", "user.name", "t"});
  std::ofstream(root + "/f.txt") << "one\n";
  git({"add", "f.txt"});
  git({"commit", "-q", "-m", "first"});
  std::ofstream(root + "/f.txt") << "one\ntwo\n";   // unstaged change
  return root;
}
// The tool runs `git` in ITS cwd — spawn it with cwd = repo via `env -C` is unavailable in
// run_subprocess, so the tool takes the repo path through git -C: it always prepends
// {"git","-C","."}; tests chdir the SUBPROCESS by passing repo as args.path? No — simplest:
// the tool runs in the agent's cwd. Tests therefore invoke the binary with cwd changed via
// a tiny sh wrapper is NOT allowed (no shell). Instead the test uses the tool's optional
// "repo" arg? NO — keep the tool cwd-based like every other tool, and have the TEST chdir:
nlohmann::json run_in(const std::string& repo, const nlohmann::json& call) {
  const std::string oldcwd = fs::current_path().string();
  fs::current_path(repo);
  ProcResult r = run_subprocess({GIT_READ_BIN}, call.dump(), 30.0);
  fs::current_path(oldcwd);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(GitReadTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({GIT_READ_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "git_read");
}

TEST(GitReadTool, StatusDiffLog) {
  const std::string repo = mk_repo("sdl");
  auto st = run_in(repo, {{"call", "git_read"}, {"args", {{"op", "status"}}}});
  ASSERT_TRUE(st.value("ok", false)) << st.dump();
  EXPECT_NE(st["result"].value("output", "").find("f.txt"), std::string::npos);

  auto di = run_in(repo, {{"call", "git_read"}, {"args", {{"op", "diff"}}}});
  ASSERT_TRUE(di.value("ok", false));
  EXPECT_NE(di["result"].value("output", "").find("+two"), std::string::npos);

  auto lo = run_in(repo, {{"call", "git_read"}, {"args", {{"op", "log"}}}});
  ASSERT_TRUE(lo.value("ok", false));
  EXPECT_NE(lo["result"].value("output", "").find("first"), std::string::npos);
}

TEST(GitReadTool, FlagInjectionAndBadOpsFailClosed) {
  const std::string repo = mk_repo("sec");
  // a path starting with '-' must be rejected, not passed to git
  auto inj = run_in(repo, {{"call", "git_read"}, {"args", {{"op", "diff"}, {"path", "--output=/tmp/pwn"}}}});
  ASSERT_FALSE(inj.is_discarded());
  EXPECT_FALSE(inj.value("ok", true));
  EXPECT_FALSE(fs::exists("/tmp/pwn"));
  auto unk = run_in(repo, {{"call", "git_read"}, {"args", {{"op", "push"}}}});
  EXPECT_FALSE(unk.value("ok", true));
  auto typed = run_in(repo, {{"call", "git_read"}, {"args", {{"op", 7}}}});
  ASSERT_FALSE(typed.is_discarded());
  EXPECT_FALSE(typed.value("ok", true));
}

TEST(GitReadTool, NotARepoReportsGitError) {
  const std::string plain = ::testing::TempDir() + "/notrepo_" + std::to_string(::getpid());
  fs::create_directories(plain);
  auto j = run_in(plain, {{"call", "git_read"}, {"args", {{"op", "status"}}}});
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));               // git exits nonzero -> ok:false + stderr text
}
