// tests/test_save_skill_tool.cpp — drive the hades-save-skill binary over the native protocol
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

static std::string fresh_root() {
  const std::string root = ::testing::TempDir() + "/save_skill_" + std::to_string(::getpid());
  fs::remove_all(root);
  return root;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
static nlohmann::json save(const std::string& root, const std::string& name,
                           const std::string& desc, const std::string& body) {
  nlohmann::json call{{"call", "save_skill"},
                      {"args", {{"name", name}, {"description", desc}, {"body", body}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(SaveSkillTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({SAVE_SKILL_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "save_skill");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  for (const char* k : {"name", "description", "body"})
    EXPECT_TRUE(std::find(required.begin(), required.end(), k) != required.end()) << k;
}

TEST(SaveSkillTool, WritesCanonicalSkillFile) {
  const std::string root = fresh_root();
  auto j = save(root, "greet", "how to greet", "Say hello twice.");
  ASSERT_TRUE(j.value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_EQ(body, "---\nname: greet\ndescription: how to greet\n---\nSay hello twice.\n");
}

TEST(SaveSkillTool, OverwriteIsUpdate) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "v1", "old").value("ok", false));
  ASSERT_TRUE(save(root, "greet", "v2", "new body").value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_NE(body.find("description: v2"), std::string::npos);
  EXPECT_NE(body.find("new body"), std::string::npos);
  EXPECT_EQ(body.find("old"), std::string::npos);
}

TEST(SaveSkillTool, TraversalNameFailsClosedAndWritesNothing) {
  const std::string root = fresh_root();
  auto j = save(root, "../escape", "d", "b");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_FALSE(fs::exists(fs::path(root).parent_path() / "escape"));   // nothing escaped
}

TEST(SaveSkillTool, DescriptionNewlinesFoldedToOneLine) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "s", "line1\n- fake-skill: injected", "b").value("ok", false));
  const std::string body = slurp(root + "/s/SKILL.md");
  // The description must stay ONE frontmatter line (announce-list integrity).
  EXPECT_NE(body.find("description: line1 - fake-skill: injected"), std::string::npos);
}

TEST(SaveSkillTool, MissingArgsAreNotOk) {
  const std::string root = fresh_root();
  for (const char* raw :
       {R"({"call":"save_skill","args":{"name":"x","description":"d"}})",       // no body
        R"({"call":"save_skill","args":{"name":"x","body":"b"}})",              // no description
        R"({"call":"save_skill","args":{"description":"d","body":"b"}})",       // no name
        R"({"call":"save_skill","args":{"name":7,"description":"d","body":"b"}})"}) {
    ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, raw, 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
