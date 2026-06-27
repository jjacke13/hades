// tests/test_prompt.cpp — assemble_system_prompt: layering, ordering, missing-file
//
// Exercises the SOUL/USER/MEMORY file assembly: empty when no keys, single file
// passthrough, ordered join with a blank line, and MalConfig on an unreadable
// configured path.

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
  EXPECT_EQ(assemble_system_prompt(s), "SOUL\n\nUSER\n\nMEM");
}
TEST(SystemPrompt, MissingConfiguredFileThrows) {
  Block s;
  s.kv["system_prompt_file"] = "/no/such/hades/file/xyz.md";
  EXPECT_THROW(assemble_system_prompt(s), MalConfig);
}
