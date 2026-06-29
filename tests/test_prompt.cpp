// tests/test_prompt.cpp — assemble_system_prompt: layering, ordering, missing-file
//
// Exercises the SOUL/USER assembly: empty when no keys, single file passthrough,
// ordered join with a blank line, MalConfig on an unreadable configured path.
// Also exercises read_memory_layer (tolerant: empty path, missing file, present file).

#include <gtest/gtest.h>
#include <fstream>
#include "hades/launcher.h"  // MalConfig
#include "hades/prompt.h"
using namespace hades;

static std::string write_tmp(const std::string& name, const std::string& content) {
  std::string p = ::testing::TempDir() + "/" + name;
  std::ofstream f(p);
  f << content;
  return p;
}

TEST(SystemPrompt, EmptyWhenNoKeys) {
  Block s;
  s.section = "Session";
  EXPECT_EQ(assemble_system_prompt(s), "");
}
TEST(SystemPrompt, SingleFilePassthrough) {
  Block s;
  s.kv["system_prompt_file"] = write_tmp("soul1.md", "You are hades.");
  EXPECT_EQ(assemble_system_prompt(s), "You are hades.");
}
TEST(SystemPrompt, LayersJoinedInOrder) {
  Block s;
  s.kv["system_prompt_file"] = write_tmp("soul2.md", "SOUL");
  s.kv["user_file"] = write_tmp("user2.md", "USER");
  s.kv["memory_file"] = write_tmp("mem2.md", "MEM");
  // memory_file is NOT assembled here — it is read live per-turn via read_memory_layer
  EXPECT_EQ(assemble_system_prompt(s), "SOUL\n\nUSER");
}
TEST(SystemPrompt, MissingConfiguredFileThrows) {
  Block s;
  s.kv["system_prompt_file"] = "/no/such/hades/file/xyz.md";
  EXPECT_THROW(assemble_system_prompt(s), MalConfig);
}

TEST(Prompt, AssembleJoinsSoulAndUserOnly) {
  const std::string soul = ::testing::TempDir() + "/soul_p.md";
  const std::string user = ::testing::TempDir() + "/user_p.md";
  const std::string mem  = ::testing::TempDir() + "/mem_p.md";
  { std::ofstream(soul) << "SOULTEXT"; }
  { std::ofstream(user) << "USERTEXT"; }
  { std::ofstream(mem)  << "MEMTEXT"; }
  Block s; s.kv["system_prompt_file"] = soul; s.kv["user_file"] = user; s.kv["memory_file"] = mem;
  std::string out = assemble_system_prompt(s);
  EXPECT_NE(out.find("SOULTEXT"), std::string::npos);
  EXPECT_NE(out.find("USERTEXT"), std::string::npos);
  EXPECT_EQ(out.find("MEMTEXT"), std::string::npos);   // memory_file is NOT assembled (read live instead)
}

TEST(Prompt, ReadMemoryLayerTolerant) {
  EXPECT_EQ(read_memory_layer(""), "");                                   // unset
  EXPECT_EQ(read_memory_layer(::testing::TempDir() + "/nope_core.md"), "");// missing file
  const std::string f = ::testing::TempDir() + "/core_present.md";
  { std::ofstream(f) << "- a fact\n"; }
  EXPECT_NE(read_memory_layer(f).find("a fact"), std::string::npos);      // present
}
