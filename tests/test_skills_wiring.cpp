// tests/test_skills_wiring.cpp — manifest-path wiring: skills module + tools end-to-end.
// Roster deliberately has NO llm module: LLM_REQUEST is captured straight off the bus, so no
// provider/api key is needed; ToolRunner executes the real save_skill binary synchronously.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;
namespace fs = std::filesystem;

static std::string fresh_root(const char* tag) {
  const std::string root =
      ::testing::TempDir() + "/skwire_" + tag + "_" + std::to_string(::getpid());
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
static std::string manifest_text(const std::string& dir) {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = skills\n" +
         "Module = arbiter\n" +
         "Tool = use_skill { native = " + USE_SKILL_BIN + " }\n" +
         "Tool = save_skill { native = " + SAVE_SKILL_BIN + " }\n" +
         "Skills\n{\n  dir = " + dir + "\n}\n";
}

TEST(SkillsWiring, AnnounceReachesLlmRequestSystemMessage) {
  const std::string root = fresh_root("announce");
  write_skill(root, "greet", "how to greet");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(root));
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.skills, nullptr);
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  const auto& msgs = req->value["messages"];
  ASSERT_EQ(msgs[0]["role"], "system");
  EXPECT_NE(msgs[0]["content"].get<std::string>().find("- greet: how to greet"),
            std::string::npos);
}

TEST(SkillsWiring, SaveSkillRoundTripRefreshesAnnounceAndWritesConfiguredDir) {
  const std::string root = fresh_root("roundtrip");
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text(root));
  Agent agent = build_agent(bb, m);
  // Drive the REAL save_skill binary through the ToolRunner (argv carries the resolved dir).
  bb.post("TOOL_REQUEST",
          {{"id", "c9"},
           {"tool", "save_skill"},
           {"args", {{"name", "newskill"}, {"description", "fresh"}, {"body", "steps"}}}},
          "arbiter");
  bb.pump();
  EXPECT_TRUE(fs::exists(root + "/newskill/SKILL.md"));   // argv append worked
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());
  EXPECT_NE(req->value["messages"][0]["content"].get<std::string>().find("- newskill: fresh"),
            std::string::npos);   // announce refreshed the same session
}

TEST(SkillsWiring, WhitespaceSkillsDirThrowsMalConfig) {
  Blackboard bb;
  Manifest m = parse_manifest(manifest_text("/tmp/has space"));
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(SkillsWiring, NoSkillsRosterLeavesAgentSkillsNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.skills, nullptr);              // no coupling: absent module is simply absent
  EXPECT_FALSE(bb.get("SKILLS_ANNOUNCE").has_value());
}
