// tests/test_use_skill_tool.cpp — drive the hades-use-skill binary over the native protocol
#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string mk_skill_root() {
  const std::string root = ::testing::TempDir() + "/use_skill_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root + "/greet");
  std::ofstream f(root + "/greet/SKILL.md");
  f << "---\nname: greet\ndescription: how to greet\n---\nSay hello twice.\n";
  return root;
}

TEST(UseSkillTool, DescribeYieldsSpec) {
  ProcResult r = run_subprocess({USE_SKILL_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "use_skill");
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  EXPECT_TRUE(std::find(required.begin(), required.end(), "name") != required.end());
}

TEST(UseSkillTool, ReturnsFullSkillContent) {
  const std::string root = mk_skill_root();
  nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", "greet"}}}};
  ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "greet");
  EXPECT_NE(j["result"].value("content", "").find("Say hello twice."), std::string::npos);
  EXPECT_NE(j["result"].value("content", "").find("description: how to greet"),
            std::string::npos);   // full file, frontmatter included
}

TEST(UseSkillTool, MissingSkillIsNotOk) {
  const std::string root = mk_skill_root();
  nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", "ghost"}}}};
  ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(UseSkillTool, TraversalNameFailsClosed) {
  const std::string root = mk_skill_root();
  // A "name" that would escape the skills dir must be rejected by the name gate,
  // NOT resolved as a path (arbitrary-file-read escape otherwise).
  for (const std::string bad : {"../greet", "a/b", "..", ".hidden", "a b"}) {
    nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", bad}}}};
    ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << bad;
    EXPECT_FALSE(j.value("ok", true)) << bad;
  }
}

TEST(UseSkillTool, NonStringNameIsNotOkAndDoesNotCrash) {
  ProcResult r = run_subprocess({USE_SKILL_BIN}, R"({"call":"use_skill","args":{"name":42}})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(UseSkillTool, InvalidUtf8ContentDoesNotCrash) {
  const std::string root = ::testing::TempDir() + "/use_skill_utf8_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root + "/binskill");
  {
    std::ofstream f(root + "/binskill/SKILL.md", std::ios::binary);
    f << "---\ndescription: d\n---\nbody \xC3\x28 \xFF\xFE trailing";   // invalid UTF-8 sequences
  }
  nlohmann::json call{{"call", "use_skill"}, {"args", {{"name", "binskill"}}}};
  ProcResult r = run_subprocess({USE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());          // ONE clean JSON line came out (no crash, no silence)
  EXPECT_TRUE(j.value("ok", false));       // read succeeded; bad bytes replaced with U+FFFD
}
