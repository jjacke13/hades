// tests/test_bridge_registry.cpp — pure bridge card builders (reverse-parse, caps summary, card)
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/bridge/registry.h"
#include "hades/config.h"   // Block
using namespace hades;

TEST(BridgeRegistry, SkillsReverseParseFromAnnounce) {
  const std::string ann =
      "Available skills (call use_skill with a name to load its full instructions):\n"
      "- deploy: ship the app\n"
      "- triage: sort incoming issues";
  auto s = build_skills_from_announce(ann);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0].value("id", ""), "deploy");
  EXPECT_EQ(s[0].value("description", ""), "ship the app");
  EXPECT_EQ(s[1].value("id", ""), "triage");
}

TEST(BridgeRegistry, SkillsReverseParseEmptyOrJunk) {
  EXPECT_TRUE(build_skills_from_announce("").empty());
  EXPECT_TRUE(build_skills_from_announce("no skill lines here").empty());
  // a header-only announce (no "- " lines) -> []
  EXPECT_TRUE(build_skills_from_announce("Available skills (…):").empty());
}

TEST(BridgeRegistry, CapsSummaryIsCategoriesNotPaths) {
  Block b;
  b.name = "capability_policy";
  b.kv["fs_read_allow"]  = "./workspace ./prompts";
  b.kv["fs_write_allow"] = "./workspace";
  b.kv["block_private_net"] = "true";
  b.kv["exec_allow"] = "cmake --build build, ctest --test-dir build";
  auto c = caps_summary(b);
  EXPECT_EQ(c.value("fs_read", ""), "scoped");
  EXPECT_EQ(c.value("fs_write", ""), "scoped");
  EXPECT_EQ(c.value("exec", ""), "scoped");
  EXPECT_EQ(c.value("net", ""), "private-blocked");
  // CRITICAL: no literal path/command leaks anywhere in the summary
  const std::string dump = c.dump();
  EXPECT_EQ(dump.find("workspace"), std::string::npos);
  EXPECT_EQ(dump.find("cmake"), std::string::npos);
}

TEST(BridgeRegistry, CapsSummaryDefaultsWhenUnset) {
  auto c = caps_summary(Block{});
  EXPECT_EQ(c.value("fs_read", ""), "none");
  EXPECT_EQ(c.value("exec", ""), "none");
  EXPECT_EQ(c.value("net", ""), "public");   // no block_private_net -> public egress
}

TEST(BridgeRegistry, BuildCardIsA2AShaped) {
  nlohmann::json tools = nlohmann::json::array({{{"name", "shell"}}, {{"name", "http_fetch"}}});
  nlohmann::json caps = {{"fs_read", "scoped"}, {"net", "public"}};
  auto card = build_card("hades2", "http://h:9090", 1, "a helper",
                         "Available skills (…):\n- deploy: ship it", tools, caps);
  EXPECT_EQ(card.value("name", ""), "hades2");
  EXPECT_EQ(card.value("description", ""), "a helper");
  EXPECT_EQ(card.value("url", ""), "http://h:9090");
  EXPECT_EQ(card.value("version", 0), 1);
  ASSERT_TRUE(card["skills"].is_array());
  EXPECT_EQ(card["skills"][0].value("id", ""), "deploy");
  EXPECT_TRUE(card["capabilities"].value("streaming", true) == false);
  EXPECT_EQ(card["tools"][0].value("name", ""), "shell");
  EXPECT_EQ(card["caps"].value("fs_read", ""), "scoped");
}
