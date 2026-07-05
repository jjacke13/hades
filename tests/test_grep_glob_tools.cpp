// tests/test_grep_glob_tools.cpp — drive hades-grep / hades-glob over the native protocol
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
std::string mk_tree(const char* tag) {
  const std::string root = ::testing::TempDir() + "/devtools_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root + "/sub/deep");
  fs::create_directories(root + "/.git");            // must be skipped
  std::ofstream(root + "/a.cpp") << "int main() {\n  return 42; // answer\n}\n";
  std::ofstream(root + "/sub/b.txt") << "hello answer world\nsecond line\n";
  std::ofstream(root + "/sub/deep/c.cpp") << "// no match here\n";
  std::ofstream(root + "/.git/blob") << "answer in git must not appear\n";
  std::ofstream(root + "/bin.dat", std::ios::binary) << std::string("an\0swer", 7);  // NUL -> binary
  return root;
}
nlohmann::json run(const char* bin, const nlohmann::json& call) {
  ProcResult r = run_subprocess({bin}, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(GrepTool, DescribeAndBasicMatch) {
  auto d = run(GREP_BIN, {{"call", "describe"}});
  ASSERT_TRUE(d.is_object() && d.value("ok", false));
  EXPECT_EQ(d["result"].value("name", ""), "grep");

  const std::string root = mk_tree("grep1");
  auto j = run(GREP_BIN, {{"call", "grep"}, {"args", {{"pattern", "answer"}, {"path", root}}}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  const auto& m = j["result"]["matches"];
  ASSERT_EQ(m.size(), 2u);                       // a.cpp + sub/b.txt; .git + binary skipped
  bool saw_cpp = false, saw_txt = false;
  for (const auto& e : m) {
    const std::string f = e.value("file", "");
    if (f.find("a.cpp") != std::string::npos) { saw_cpp = true; EXPECT_EQ(e.value("line", 0), 2); }
    if (f.find("b.txt") != std::string::npos) saw_txt = true;
    EXPECT_EQ(f.find(".git"), std::string::npos);
  }
  EXPECT_TRUE(saw_cpp);
  EXPECT_TRUE(saw_txt);
}

TEST(GrepTool, IgnoreCaseAndRegex) {
  const std::string root = mk_tree("grep2");
  auto j = run(GREP_BIN, {{"call", "grep"},
                          {"args", {{"pattern", "^HELLO"}, {"path", root}, {"ignore_case", true}}}});
  ASSERT_TRUE(j.value("ok", false));
  ASSERT_EQ(j["result"]["matches"].size(), 1u);  // sub/b.txt line 1
}

TEST(GrepTool, InvalidRegexAndBadArgsFailClosed) {
  const std::string root = mk_tree("grep3");
  auto bad = run(GREP_BIN, {{"call", "grep"}, {"args", {{"pattern", "([unclosed"}, {"path", root}}}});
  ASSERT_FALSE(bad.is_discarded());
  EXPECT_FALSE(bad.value("ok", true));
  auto missing = run(GREP_BIN, {{"call", "grep"}, {"args", {{"path", root}}}});
  EXPECT_FALSE(missing.value("ok", true));
  auto typed = run(GREP_BIN, {{"call", "grep"}, {"args", {{"pattern", 42}, {"path", root}}}});
  ASSERT_FALSE(typed.is_discarded());
  EXPECT_FALSE(typed.value("ok", true));
  auto nodir = run(GREP_BIN, {{"call", "grep"}, {"args", {{"pattern", "x"}, {"path", root + "/nope"}}}});
  EXPECT_FALSE(nodir.value("ok", true));
}

TEST(GrepTool, MaxResultsTruncates) {
  const std::string root = mk_tree("grep4");
  std::ofstream big(root + "/many.txt");
  for (int i = 0; i < 50; ++i) big << "answer " << i << "\n";
  big.close();
  auto j = run(GREP_BIN, {{"call", "grep"},
                          {"args", {{"pattern", "answer"}, {"path", root}, {"max_results", 5}}}});
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"]["matches"].size(), 5u);
  EXPECT_TRUE(j["result"].value("truncated", false));
}

TEST(GrepTool, ContextAndOutputCap) {
  const std::string root = ::testing::TempDir() + "/devtools_grepctx_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  // Context inclusion: matched line's text carries its neighbours.
  std::ofstream(root + "/ctx.txt") << "line one alpha\nline two target\nline three gamma\n";
  auto c = run(GREP_BIN, {{"call", "grep"},
                          {"args", {{"pattern", "target"}, {"path", root + "/ctx.txt"},
                                    {"context", 1}}}});
  ASSERT_TRUE(c.value("ok", false)) << c.dump();
  ASSERT_EQ(c["result"]["matches"].size(), 1u);
  const std::string text = c["result"]["matches"][0].value("text", "");
  EXPECT_NE(text.find("alpha"), std::string::npos);   // preceding context line
  EXPECT_NE(text.find("gamma"), std::string::npos);   // following context line

  // Output cap: ~200 matches of 400-char lines + context=5 must truncate under 64KB.
  const std::string longline = std::string(400, 'X') + " MATCH\n";
  std::ofstream big(root + "/big.txt");
  for (int i = 0; i < 200; ++i) big << longline;
  big.close();
  auto j = run(GREP_BIN, {{"call", "grep"},
                          {"args", {{"pattern", "MATCH"}, {"path", root + "/big.txt"},
                                    {"context", 5}, {"max_results", 500}}}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_TRUE(j["result"].value("truncated", false));
  EXPECT_LT(j.dump().size(), 96u * 1024u);
  fs::remove_all(root);
}

TEST(GlobTool, OutputByteCapTruncates) {
  const std::string root = ::testing::TempDir() + "/devtools_globcap_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  // Long file names so accumulated path bytes exceed 64KB well before max_results=1000.
  const std::string pad(240, 'x');
  for (int i = 0; i < 400; ++i) std::ofstream(root + "/file_" + std::to_string(i) + pad);
  auto j = run(GLOB_BIN, {{"call", "glob"},
                          {"args", {{"pattern", "*"}, {"path", root}, {"max_results", 1000}}}});
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_TRUE(j["result"].value("truncated", false));
  EXPECT_FALSE(j["result"]["files"].empty());
  fs::remove_all(root);
}

TEST(GlobTool, DescribeAndPatterns) {
  auto d = run(GLOB_BIN, {{"call", "describe"}});
  ASSERT_TRUE(d.value("ok", false));
  EXPECT_EQ(d["result"].value("name", ""), "glob");

  const std::string root = mk_tree("glob1");
  auto all_cpp = run(GLOB_BIN, {{"call", "glob"}, {"args", {{"pattern", "**/*.cpp"}, {"path", root}}}});
  ASSERT_TRUE(all_cpp.value("ok", false)) << all_cpp.dump();
  ASSERT_EQ(all_cpp["result"]["files"].size(), 2u);          // a.cpp + sub/deep/c.cpp; sorted
  EXPECT_NE(all_cpp["result"]["files"][0].get<std::string>().find("a.cpp"), std::string::npos);

  auto top_only = run(GLOB_BIN, {{"call", "glob"}, {"args", {{"pattern", "*.cpp"}, {"path", root}}}});
  ASSERT_EQ(top_only["result"]["files"].size(), 1u);         // '*' does not cross '/'

  auto qmark = run(GLOB_BIN, {{"call", "glob"}, {"args", {{"pattern", "?.cpp"}, {"path", root}}}});
  ASSERT_EQ(qmark["result"]["files"].size(), 1u);            // a.cpp

  auto none = run(GLOB_BIN, {{"call", "glob"}, {"args", {{"pattern", "*.rs"}, {"path", root}}}});
  ASSERT_TRUE(none.value("ok", false));
  EXPECT_TRUE(none["result"]["files"].empty());
}

TEST(GlobTool, BadArgsFailClosed) {
  auto missing = run(GLOB_BIN, {{"call", "glob"}, {"args", nlohmann::json::object()}});
  ASSERT_FALSE(missing.is_discarded());
  EXPECT_FALSE(missing.value("ok", true));
  auto nodir = run(GLOB_BIN, {{"call", "glob"}, {"args", {{"pattern", "*"}, {"path", "/nope/nope"}}}});
  EXPECT_FALSE(nodir.value("ok", true));
}
