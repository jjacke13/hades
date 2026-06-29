// tests/test_memory_rank.cpp — pure keyword ranker: overlap scoring, recency tie, top_n cap
#include <gtest/gtest.h>
#include "hades/memory/rank.h"
using namespace hades;

TEST(MemoryRank, ScoresOverlapDropsZero) {
  std::vector<MemoryRecord> all = {
      {"the cat sat on the mat", 1.0},
      {"dogs are loyal animals", 2.0},      // no exact token overlap with "cat dog" -> dropped
      {"a cat and a dog played", 3.0},
  };
  auto top = rank_memories(all, "cat dog", 5);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "a cat and a dog played");  // score 2 wins
  EXPECT_EQ(top[1].text, "the cat sat on the mat");  // score 1
}

TEST(MemoryRank, RecencyTieBreakAndTopNCap) {
  std::vector<MemoryRecord> all = {{"cat one", 1.0}, {"cat two", 5.0}, {"cat three", 3.0}};
  auto top = rank_memories(all, "cat", 2);
  ASSERT_EQ(top.size(), 2u);              // capped at top_n
  EXPECT_EQ(top[0].text, "cat two");      // newest first on equal score
  EXPECT_EQ(top[1].text, "cat three");
}

TEST(MemoryRank, EmptyQueryYieldsNothing) {
  std::vector<MemoryRecord> all = {{"cat", 1.0}};
  EXPECT_TRUE(rank_memories(all, "", 5).empty());
}
