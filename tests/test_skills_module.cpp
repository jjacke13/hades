// tests/test_skills_module.cpp — SkillsModule: announce at attach, event-driven rescan
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/blackboard.h"
#include "hades/module/skills_module.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_root(const char* tag) {
  const std::string root =
      ::testing::TempDir() + "/skmod_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}
static void write_skill(const std::string& root, const std::string& name,
                        const std::string& desc) {
  fs::create_directories(root + "/" + name);
  std::ofstream f(root + "/" + name + "/SKILL.md");
  f << "---\ndescription: " << desc << "\n---\nbody\n";
}
static void attach(SkillsModule& m, Blackboard& bb, const std::string& dir) {
  Block cfg;
  cfg.kv["dir"] = dir;
  m.on_start(cfg, bb);
  m.on_attach(bb);
}
static std::string announce(Blackboard& bb) {
  auto e = bb.get("SKILLS_ANNOUNCE");
  return (e && e->value.is_string()) ? e->value.get<std::string>() : "<missing>";
}

TEST(SkillsModule, PostsAnnounceOnAttach) {
  const std::string root = fresh_root("attach");
  write_skill(root, "beta", "second");
  write_skill(root, "alpha", "first");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  const std::string a = announce(bb);
  EXPECT_NE(a.find("Available skills"), std::string::npos);
  EXPECT_LT(a.find("- alpha: first"), a.find("- beta: second"));   // sorted
}

TEST(SkillsModule, EmptyDirPostsEmptyString) {
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, fresh_root("empty"));
  EXPECT_EQ(announce(bb), "");
}

TEST(SkillsModule, RescansAfterSuccessfulSaveSkill) {
  const std::string root = fresh_root("rescan");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  ASSERT_EQ(announce(bb), "");
  write_skill(root, "fresh", "just saved");   // what the save_skill tool would have written
  bb.post("TOOL_REQUEST", {{"id", "s1"}, {"tool", "save_skill"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "s1"}, {"ok", true}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_NE(announce(bb).find("- fresh: just saved"), std::string::npos);
}

TEST(SkillsModule, NoRescanOnOtherToolsOrFailedSave) {
  const std::string root = fresh_root("norescan");
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, root);
  write_skill(root, "sneaky", "should not appear yet");
  // other tool succeeds -> no rescan
  bb.post("TOOL_REQUEST", {{"id", "f1"}, {"tool", "fs_read"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "f1"}, {"ok", true}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_EQ(announce(bb), "");
  // save_skill FAILS -> no rescan
  bb.post("TOOL_REQUEST", {{"id", "s2"}, {"tool", "save_skill"}, {"args", {}}}, "arbiter");
  bb.post("TOOL_RESULT", {{"id", "s2"}, {"ok", false}, {"content", {}}}, "tool_runner");
  bb.pump();
  EXPECT_EQ(announce(bb), "");
}

TEST(SkillsModule, MalformedBusPayloadsDoNotCrash) {
  Blackboard bb;
  SkillsModule m;
  attach(m, bb, fresh_root("malformed"));
  bb.post("TOOL_REQUEST", "not an object", "x");
  bb.post("TOOL_RESULT", 42, "x");
  bb.pump();   // must not throw
  SUCCEED();
}
