// tests/test_memory_store.cpp — JSONL store loader: valid lines kept, junk skipped, missing -> empty
#include <gtest/gtest.h>
#include <fstream>
#include "hades/memory/store.h"
using namespace hades;

TEST(MemoryStore, LoadsValidSkipsMalformed) {
  const std::string path = ::testing::TempDir() + "/store.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"alpha","ts":1.5})" << "\n";
    f << "not json at all" << "\n";          // malformed -> skip
    f << R"({"ts":2.0})" << "\n";            // missing text -> skip
    f << R"({"text":"beta","ts":3.0})" << "\n";
  }
  auto v = load_memories(path);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].text, "alpha");
  EXPECT_DOUBLE_EQ(v[0].ts, 1.5);
  EXPECT_EQ(v[1].text, "beta");
  EXPECT_DOUBLE_EQ(v[1].ts, 3.0);
}

TEST(MemoryStore, MissingFileIsEmpty) {
  EXPECT_TRUE(load_memories(::testing::TempDir() + "/no_such_store.jsonl").empty());
}

TEST(MemoryStore, NonNumericTsDoesNotThrow) {
  const std::string path = ::testing::TempDir() + "/store_badts.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"keep","ts":"not-a-number"})" << "\n";
  }
  std::vector<MemoryRecord> v;
  ASSERT_NO_THROW(v = load_memories(path));
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].text, "keep");
  EXPECT_DOUBLE_EQ(v[0].ts, 0.0);
}
