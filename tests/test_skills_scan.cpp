// tests/test_skills_scan.cpp — pure skills scan lib: frontmatter, name gate, scan, announce
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/skills/scan.h"
using namespace hades;
namespace fs = std::filesystem;

static void write_skill(const std::string& root, const std::string& name,
                        const std::string& content) {
  fs::create_directories(root + "/" + name);
  std::ofstream f(root + "/" + name + "/SKILL.md");
  f << content;
}

TEST(SkillsScan, ParsesDescriptionFromFrontmatter) {
  EXPECT_EQ(parse_skill_description("---\nname: x\ndescription: does things\n---\nbody"),
            "does things");
  EXPECT_EQ(parse_skill_description("---\ndescription:   padded   \n---\n"), "padded");
}

TEST(SkillsScan, UnparseableYieldsEmpty) {
  EXPECT_EQ(parse_skill_description(""), "");
  EXPECT_EQ(parse_skill_description("no frontmatter at all"), "");
  EXPECT_EQ(parse_skill_description("---\ndescription: never closed"), "");   // no closing fence
  EXPECT_EQ(parse_skill_description("---\nname: x\n---\nbody"), "");          // no description
}

TEST(SkillsScan, ValidSkillNameGate) {
  EXPECT_TRUE(valid_skill_name("deploy-webapp_2"));
  EXPECT_TRUE(valid_skill_name("A"));
  EXPECT_FALSE(valid_skill_name(""));
  EXPECT_FALSE(valid_skill_name("../escape"));
  EXPECT_FALSE(valid_skill_name("a/b"));
  EXPECT_FALSE(valid_skill_name("a\\b"));
  EXPECT_FALSE(valid_skill_name("a b"));
  EXPECT_FALSE(valid_skill_name("dot.name"));
  EXPECT_FALSE(valid_skill_name(std::string(65, 'a')));   // length cap 64
}

TEST(SkillsScan, ScansSortedAndSkipsBadEntries) {
  const std::string root = ::testing::TempDir() + "/skills_scan_" + std::to_string(::getpid());
  fs::remove_all(root);
  write_skill(root, "zeta", "---\ndescription: last\n---\nz");
  write_skill(root, "alpha", "---\ndescription: first\n---\na");
  write_skill(root, "broken", "no frontmatter");            // skipped: unparseable
  fs::create_directories(root + "/empty-dir");              // skipped: no SKILL.md
  auto v = scan_skills_dir(root);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].name, "alpha");
  EXPECT_EQ(v[0].description, "first");
  EXPECT_EQ(v[1].name, "zeta");
}

TEST(SkillsScan, MissingDirYieldsEmpty) {
  EXPECT_TRUE(scan_skills_dir("/nonexistent/skills/dir").empty());
}

TEST(SkillsScan, FormatAnnounce) {
  EXPECT_EQ(format_skills_announce({}), "");
  std::string a = format_skills_announce({{"alpha", "first"}, {"zeta", "last"}});
  EXPECT_EQ(a,
            "Available skills (call use_skill with a name to load its full instructions):\n"
            "- alpha: first\n"
            "- zeta: last");
}

TEST(SkillsScan, ResolveSkillsDir) {
  EXPECT_EQ(resolve_skills_dir(Block{}), "skills");
  Block b; b.kv["dir"] = "my/skilldir";
  EXPECT_EQ(resolve_skills_dir(b), "my/skilldir");
}
