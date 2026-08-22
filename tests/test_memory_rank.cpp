// tests/test_memory_rank.cpp — pure keyword ranker: overlap scoring, recency tie, top_n cap
#include <limits>
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

// ── supersession + blended ranking (2026-08-22) ────────────────────────────────
// All new tests pin `now` via the 4-arg overload so scoring is deterministic.
namespace {
constexpr double kDay = 86400.0;
constexpr double kNow = 1'000'000'000.0;   // fixed reference "now" for these tests
}  // namespace

TEST(MemoryRank, NewerTopicRecordSupersedesOlder) {
  std::vector<MemoryRecord> all = {
      {"prefers aisle seats", kNow - 10 * kDay, "seat-pref"},
      {"prefers window seats", kNow - 1 * kDay, "seat-pref"},
  };
  auto top = rank_memories(all, "prefers seats", 5, kNow);
  ASSERT_EQ(top.size(), 1u);                       // stale value suppressed entirely
  EXPECT_EQ(top[0].text, "prefers window seats");
}

TEST(MemoryRank, UntopicedRecordsNeverSupersedeEachOther) {
  std::vector<MemoryRecord> all = {
      {"cat one", kNow - 10 * kDay},               // topic defaults to ""
      {"cat two", kNow - 1 * kDay},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  EXPECT_EQ(top.size(), 2u);                       // both kept: "" is not a bucket
}

TEST(MemoryRank, DistinctTopicsCoexist) {
  std::vector<MemoryRecord> all = {
      {"prefers window seats", kNow - 1 * kDay, "seat-pref"},
      {"prefers vegetarian meals", kNow - 2 * kDay, "meal-pref"},
  };
  auto top = rank_memories(all, "prefers", 5, kNow);
  EXPECT_EQ(top.size(), 2u);
}

TEST(MemoryRank, RelevanceStillDominatesRecency) {
  // Old but fully-matching beats brand-new but weakly-matching.
  std::vector<MemoryRecord> all = {
      {"alpha beta", 0.0},                          // rel 2/2 = 1.0, recency ~0
      {"alpha gamma delta", kNow},                  // rel 1/2 = 0.5, recency 1.0
  };
  auto top = rank_memories(all, "alpha beta", 5, kNow);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "alpha beta");             // 1.0 vs 0.5+0.3 = 0.8
}

TEST(MemoryRank, RecencyReordersEqualRelevance) {
  std::vector<MemoryRecord> all = {
      {"cat alpha", kNow - 365 * kDay},
      {"cat beta", kNow - 1 * kDay},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "cat beta");               // same relevance, newer wins
}

TEST(MemoryRank, ReinforcementLiftsRestatedTopic) {
  // Same relevance and same ts for the two survivors; the restated topic wins on
  // reinforcement (bucket of 3 -> 0.75 vs untopiced 0.5).
  const double t = kNow - 2 * kDay;
  std::vector<MemoryRecord> all = {
      {"cat restated", t, "cat-topic"},
      {"cat restated older", t - kDay, "cat-topic"},
      {"cat restated oldest", t - 2 * kDay, "cat-topic"},
      {"cat standalone", t},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(top.size(), 2u);                        // 3 topic records collapse to 1
  EXPECT_EQ(top[0].text, "cat restated");
}

TEST(MemoryRank, TieBreakIsFullyDeterministic) {
  // Identical relevance, ts and topic-state -> text ascending, stable across runs.
  std::vector<MemoryRecord> all = {
      {"cat zulu", kNow}, {"cat alpha", kNow}, {"cat mike", kNow}};
  auto a = rank_memories(all, "cat", 5, kNow);
  auto b = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0].text, "cat alpha");
  EXPECT_EQ(a[1].text, "cat mike");
  EXPECT_EQ(a[2].text, "cat zulu");
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].text, b[i].text);
}

TEST(MemoryRank, SupersessionRespectsTopNCap) {
  std::vector<MemoryRecord> all = {
      {"cat a", kNow - 3 * kDay, "t1"}, {"cat a old", kNow - 9 * kDay, "t1"},
      {"cat b", kNow - 2 * kDay, "t2"}, {"cat c", kNow - 1 * kDay, "t3"}};
  auto top = rank_memories(all, "cat", 2, kNow);
  ASSERT_EQ(top.size(), 2u);                        // cap applied AFTER supersession
  // "cat a old" is superseded, so the cap cuts "cat b" — not the stale record. Order is the
  // blend: t1 was restated (bucket 2 -> +0.133) which outweighs cat c's 2-day recency edge
  // (+0.013), so 1.4132 > 1.3932 > 1.3864 (cat b).
  EXPECT_EQ(top[0].text, "cat a");
  EXPECT_EQ(top[1].text, "cat c");
}

TEST(MemoryRank, ThreeArgOverloadStillWorks) {
  // Clock-backed overload: old records, so recency ~0 and relevance decides.
  std::vector<MemoryRecord> all = {{"cat sat", 1.0}, {"dog ran", 2.0}};
  auto top = rank_memories(all, "cat", 5);
  ASSERT_EQ(top.size(), 1u);
  EXPECT_EQ(top[0].text, "cat sat");
}

// ── review findings I1 + I2 (2026-08-22) ──────────────────────────────────────
TEST(MemoryRank, SubstantiveOldBeatsIncidentalFreshOnLongQuery) {
  // I1 regression. The live MemoryModule passes the WHOLE user message as the query, so
  // |q| is large and each matched token is worth only 1/|q| of relevance. Under an ADDITIVE
  // blend a fixed +0.3 recency bonus swamped that: one incidental fresh match outranked four
  // substantive old ones. The multiplier form must keep the substantive record first.
  std::vector<MemoryRecord> all = {
      {"user deploys with nix flakes on nixos and pins nixpkgs release", kNow - 365 * kDay},
      {"today the weather is nice", kNow},
  };
  auto top = rank_memories(all, "how do I deploy my nixos config with nix flakes today", 5, kNow);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "user deploys with nix flakes on nixos and pins nixpkgs release");
}

TEST(MemoryRank, TerseCorrectionCanHideTheWholeTopic) {
  // I2: documented, deliberate behavior — NOT a bug to "fix" by resurrecting the old record.
  // The superseded value must never come back (that is the whole point of supersession), so a
  // terse replacement that shares no token with the query yields nothing for that topic.
  // The mitigation is behavioral: soul.md tells the agent to keep corrections self-contained.
  std::vector<MemoryRecord> all = {
      {"prefers aisle seats on flights", kNow - 30 * kDay, "seat-pref"},
      {"window", kNow - 1 * kDay, "seat-pref"},
  };
  auto top = rank_memories(all, "what seat do I prefer on flights", 5, kNow);
  EXPECT_TRUE(top.empty());   // stale suppressed; terse survivor does not match the query
  // A self-contained correction is retrievable, same query:
  all[1].text = "prefers window seats on flights";
  auto top2 = rank_memories(all, "what seat do I prefer on flights", 5, kNow);
  ASSERT_EQ(top2.size(), 1u);
  EXPECT_EQ(top2[0].text, "prefers window seats on flights");
}

TEST(MemoryRank, NonFiniteNowDoesNotPoisonTheSort) {
  // A NaN score makes the comparator a non-strict-weak-ordering -> UB in std::sort.
  std::vector<MemoryRecord> all = {
      {"cat alpha", kNow}, {"cat beta", kNow - kDay}, {"cat gamma", kNow - 2 * kDay}};
  auto top = rank_memories(all, "cat", 5, std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(top.size(), 3u);   // degrades to no-recency-information, never UB
}

TEST(MemoryRank, NonFiniteRecordTsDoesNotPoisonTheSort) {
  // Mirror of NonFiniteNowDoesNotPoisonTheSort on the other operand: a caller-built record
  // with NaN/inf ts must not make the comparator a non-strict-weak-ordering (UB in std::sort).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<MemoryRecord> all = {
      {"cat nan", nan}, {"cat inf", inf}, {"cat neginf", -inf}, {"cat sane", kNow - kDay}};
  auto top = rank_memories(all, "cat", 10, kNow);
  EXPECT_EQ(top.size(), 4u);   // all admitted, deterministic order, no UB
}
