#include <gtest/gtest.h>
#include <string>
#include "hades/embedding/vector_cache.h"
using namespace hades;

static std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }

TEST(VectorCache, PutLoadRoundTripAndHas) {
  std::string p = tmp("vc_rt.jsonl");
  std::remove(p.c_str());
  { VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
    c.put({"memory#0", "memory", "alpha", {1.0f, 0.0f}});
    c.put({"memory#1", "memory", "beta",  {0.0f, 1.0f}}); }
  VectorCache c2(p, "echo", 2);
  ASSERT_TRUE(c2.load());
  EXPECT_EQ(c2.size(), 2u);
  EXPECT_TRUE(c2.has("memory#0"));
  EXPECT_FALSE(c2.has("memory#99"));
}
TEST(VectorCache, QueryRanksByCosineAboveFloor) {
  std::string p = tmp("vc_q.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"a", "memory", "alpha", {1.0f, 0.0f}});
  c.put({"b", "memory", "beta",  {0.0f, 1.0f}});
  auto top = c.query({0.9f, 0.1f}, 5, 0.2f);   // closest to "alpha"
  ASSERT_FALSE(top.empty());
  EXPECT_EQ(top[0].text, "alpha");
  // a query 45 deg from both stored vecs -> cosine 0.707 < floor 0.9 -> nothing
  EXPECT_TRUE(c.query({1.0f, 1.0f}, 5, 0.9f).empty());
}
TEST(VectorCache, QueryRanksMultipleHitsAndCapsTopN) {
  // The retrieval core: >=2 above-floor hits must come back sorted by cosine desc, and top_n must
  // truncate to the top-N (cap applied AFTER ranking). Three records at increasing angles from the
  // query, all above a low floor.
  std::string p = tmp("vc_rank.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"near",  "memory", "near",  {1.0f, 0.05f}});   // ~highest cosine to {1,0}
  c.put({"mid",   "memory", "mid",   {1.0f, 0.5f}});    // middle
  c.put({"far",   "memory", "far",   {1.0f, 1.0f}});    // lowest (0.707)
  auto all = c.query({1.0f, 0.0f}, 5, 0.5f);            // floor admits all three
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].text, "near");                       // sorted by cosine desc
  EXPECT_EQ(all[1].text, "mid");
  EXPECT_EQ(all[2].text, "far");
  EXPECT_GE(all[0].score, all[1].score);
  EXPECT_GE(all[1].score, all[2].score);
  auto capped = c.query({1.0f, 0.0f}, 2, 0.5f);         // top_n=2 truncates after ranking
  ASSERT_EQ(capped.size(), 2u);
  EXPECT_EQ(capped[0].text, "near");
  EXPECT_EQ(capped[1].text, "mid");                     // "far" dropped by the cap, not the floor
}
TEST(VectorCache, ModelMismatchFailsLoadForRebuild) {
  std::string p = tmp("vc_mm.jsonl");
  std::remove(p.c_str());
  { VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load()); c.put({"x", "memory", "t", {1.0f, 0.0f}}); }
  VectorCache other(p, "DIFFERENT-MODEL", 2);
  EXPECT_FALSE(other.load());                   // stamp mismatch -> caller rebuilds
  EXPECT_EQ(other.size(), 0u);                  // mem_ left EMPTY on mismatch (not partial)
}
TEST(VectorCache, MissingFileLoadsEmpty) {
  VectorCache c(tmp("vc_none.jsonl"), "echo", 2);
  EXPECT_TRUE(c.load());
  EXPECT_EQ(c.size(), 0u);
}
TEST(VectorCache, DegenerateZeroVectorDropped) {
  std::string p = tmp("vc_zero.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"z", "memory", "zero", {0.0f, 0.0f}});  // normalize fails -> dropped
  EXPECT_EQ(c.size(), 0u);
}
TEST(VectorCache, QueryCarriesSrc) {
  std::string p = tmp("vc_src.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"m0", "memory",  "a fact",     {1.0f, 0.0f}});
  c.put({"s0", "session", "U: hi\nA: yo", {0.0f, 1.0f}});
  auto fa = c.query({1.0f, 0.05f}, 5, 0.1f);   // closest to the memory record
  ASSERT_FALSE(fa.empty());
  EXPECT_EQ(fa[0].text, "a fact");
  EXPECT_EQ(fa[0].src, "memory");
  auto se = c.query({0.05f, 1.0f}, 5, 0.1f);   // closest to the session record
  ASSERT_FALSE(se.empty());
  EXPECT_EQ(se[0].src, "session");
}
