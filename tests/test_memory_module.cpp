// tests/test_memory_module.cpp — USER_MESSAGE -> load store, rank, post RETRIEVED_MEMORY
#include <gtest/gtest.h>
#include <fstream>
#include "hades/module/memory_module.h"
#include "hades/blackboard.h"
using namespace hades;

TEST(MemoryModule, PostsMatchingMemoryExcludesNonMatching) {
  const std::string path = ::testing::TempDir() + "/mm.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"user prefers tea over coffee","ts":1.0})" << "\n";
    f << R"({"text":"project deadline is friday","ts":2.0})" << "\n";
  }
  Blackboard bb;
  MemoryModule m;
  Block cfg;
  cfg.kv["store"] = path;
  cfg.kv["top_n"] = "5";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "what tea do i like", "chat");
  bb.pump();
  EXPECT_NE(got.find("tea"), std::string::npos);
  EXPECT_EQ(got.find("deadline"), std::string::npos);  // non-matching record excluded
}

TEST(MemoryModule, EmptyStringWhenNoMatchOrNoStore) {
  Blackboard bb;
  MemoryModule m;
  Block cfg;
  cfg.kv["store"] = ::testing::TempDir() + "/mm_absent.jsonl";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "anything", "chat");
  bb.pump();
  EXPECT_EQ(got, "");
}

TEST(MemoryModule, IgnoresNonStringUserMessage) {
  Blackboard bb;
  MemoryModule m;
  Block cfg;
  cfg.kv["store"] = ::testing::TempDir() + "/mm_nonstring.jsonl";
  m.on_start(cfg, bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.is_string() ? e.value.get<std::string>() : "NONSTRING"; });
  bb.post("USER_MESSAGE", 42, "chat");   // integer payload, not a string
  bb.pump();
  EXPECT_EQ(got, "UNSET");   // module ignored it -> never posted RETRIEVED_MEMORY
}
