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
TEST(VectorCache, ModelMismatchFailsLoadForRebuild) {
  std::string p = tmp("vc_mm.jsonl");
  std::remove(p.c_str());
  { VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load()); c.put({"x", "memory", "t", {1.0f, 0.0f}}); }
  VectorCache other(p, "DIFFERENT-MODEL", 2);
  EXPECT_FALSE(other.load());                   // stamp mismatch -> caller rebuilds
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
