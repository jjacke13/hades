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
  // Only name is unconditionally required: body selects save mode, old_string selects patch
  // mode, and the runtime dispatch enforces the per-mode arg sets.
  auto required = j["result"]["schema"].value("required", nlohmann::json::array());
  ASSERT_EQ(required.size(), 1u);
  EXPECT_EQ(required[0], "name");
  const auto& props = j["result"]["schema"]["properties"];
  for (const char* k : {"name", "description", "body", "old_string", "new_string"})
    EXPECT_TRUE(props.contains(k)) << k;
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

// Patch-mode driver: fills EVERY schema field (empty strings for the unused mode's args) the
// way weak LLMs do — so every patch test also pins the empty-string-is-absent rule.
static nlohmann::json patch(const std::string& root, const std::string& name,
                            const std::string& olds, const std::string& news,
                            const std::string& desc = "") {
  nlohmann::json call{{"call", "save_skill"},
                      {"args",
                       {{"name", name},
                        {"description", desc},
                        {"body", ""},
                        {"old_string", olds},
                        {"new_string", news}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}

TEST(SaveSkillTool, PatchReplacesExactlyOnce) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "how to greet", "Say hello twice.\nThen wave.").value("ok", false));
  auto j = patch(root, "greet", "hello twice", "hi once");
  ASSERT_TRUE(j.value("ok", false));
  EXPECT_TRUE(j["result"].value("patched", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_NE(body.find("Say hi once."), std::string::npos);
  EXPECT_EQ(body.find("hello twice"), std::string::npos);
  EXPECT_NE(body.find("description: how to greet"), std::string::npos);   // frontmatter intact
}

TEST(SaveSkillTool, PatchEmptyNewStringDeletes) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "Say hello twice.\nThen wave.\n").value("ok", false));
  ASSERT_TRUE(patch(root, "greet", "\nThen wave.", "").value("ok", false));
  const std::string body = slurp(root + "/greet/SKILL.md");
  EXPECT_EQ(body.find("Then wave"), std::string::npos);
  EXPECT_NE(body.find("Say hello twice."), std::string::npos);
}

TEST(SaveSkillTool, PatchOldStringNotFoundFailsAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "never there", "x");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchAmbiguousMatchFailsAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "alpha beta alpha").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "alpha", "gamma");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("2 times"), std::string::npos);
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, BothModesFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body").value("ok", false));
  nlohmann::json call{{"call", "save_skill"},
                      {"args",
                       {{"name", "greet"}, {"description", "d"}, {"body", "new body"},
                        {"old_string", "body"}, {"new_string", "x"}}}};
  ProcResult r = run_subprocess({SAVE_SKILL_BIN, root}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("not both"), std::string::npos);
}

TEST(SaveSkillTool, NeitherModeFails) {
  const std::string root = fresh_root();
  // Every field present but empty = every field absent (weak-LLM shape).
  auto j = patch(root, "greet", "", "");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveSkillTool, PatchWithDescriptionFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  auto j = patch(root, "greet", "body text", "new text", "a new description");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchBreakingFrontmatterRefusedAndFileUntouched) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "how to greet", "body text").value("ok", false));
  const std::string before = slurp(root + "/greet/SKILL.md");
  // Patching the description KEY away would make the scanner drop the skill -> refuse.
  auto j = patch(root, "greet", "description:", "junk:");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("frontmatter"), std::string::npos);
  EXPECT_EQ(slurp(root + "/greet/SKILL.md"), before);
}

TEST(SaveSkillTool, PatchCanEditFrontmatterDescriptionValue) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "old description", "body text").value("ok", false));
  // Editing the description VALUE (key intact) is legal patch usage.
  ASSERT_TRUE(patch(root, "greet", "description: old description",
                    "description: better one-liner").value("ok", false));
  EXPECT_NE(slurp(root + "/greet/SKILL.md").find("description: better one-liner"),
            std::string::npos);
}

TEST(SaveSkillTool, PatchMissingSkillFails) {
  const std::string root = fresh_root();
  auto j = patch(root, "ghost", "a", "b");
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("no such skill"), std::string::npos);
}

TEST(SaveSkillTool, PatchTraversalNameFailsClosed) {
  const std::string root = fresh_root();
  auto j = patch(root, "../escape", "a", "b");
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(SaveSkillTool, PatchIdenticalStringsFails) {
  const std::string root = fresh_root();
  ASSERT_TRUE(save(root, "greet", "d", "body").value("ok", false));
  auto j = patch(root, "greet", "body", "body");
  EXPECT_FALSE(j.value("ok", true));
}
