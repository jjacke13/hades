// tests/test_when.cpp — pure when-condition lib: parse, validate, evaluate
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/when.h"
using namespace hades;

TEST(When, ParsesAllFiveForms) {
  auto c = parse_when("PEER.pi0.card changes");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->key, "PEER.pi0.card");
  EXPECT_EQ(c->op, WhenCond::Op::Changes);

  c = parse_when("MISSION_STATE is returning home");   // operand may contain spaces
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Is);
  EXPECT_EQ(c->operand, "returning home");

  c = parse_when("MISSION_STATE not idle");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Not);

  c = parse_when("BUDGET_SPENT_USD above 0.8");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Above);
  EXPECT_EQ(c->operand, "0.8");

  c = parse_when("GPS_QUALITY below 4");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->op, WhenCond::Op::Below);
}

TEST(When, RejectsMalformed) {
  EXPECT_FALSE(parse_when("").has_value());
  EXPECT_FALSE(parse_when("KEY").has_value());                    // no op
  EXPECT_FALSE(parse_when("KEY frobnicates").has_value());        // unknown op
  EXPECT_FALSE(parse_when("KEY changes extra").has_value());      // changes takes no operand
  EXPECT_FALSE(parse_when("KEY is").has_value());                 // is needs an operand
  EXPECT_FALSE(parse_when("KEY above").has_value());              // threshold needs an operand
  EXPECT_FALSE(parse_when("KEY above lots").has_value());         // non-numeric threshold
  EXPECT_TRUE(when_valid("K is v"));
  EXPECT_FALSE(when_valid("K above nan-ish"));
}

TEST(When, HoldsStringEquality) {
  WhenCond c{"K", WhenCond::Op::Is, "idle"};
  nlohmann::json s = "idle";
  nlohmann::json other = "busy";
  EXPECT_TRUE(when_holds(c, &s));
  EXPECT_FALSE(when_holds(c, &other));
  EXPECT_FALSE(when_holds(c, nullptr));                            // absent key -> false
  c.op = WhenCond::Op::Not;
  EXPECT_FALSE(when_holds(c, &s));
  EXPECT_TRUE(when_holds(c, &other));
  EXPECT_FALSE(when_holds(c, nullptr));                            // absent: not even "not"
}

TEST(When, HoldsNonStringValuesCompareAsDump) {
  WhenCond c{"K", WhenCond::Op::Is, "{\"a\":1}"};
  nlohmann::json obj = {{"a", 1}};
  EXPECT_TRUE(when_holds(c, &obj));                                // compact dump equality
}

TEST(When, HoldsNumericThresholds) {
  WhenCond above{"K", WhenCond::Op::Above, "0.8"};
  WhenCond below{"K", WhenCond::Op::Below, "4"};
  nlohmann::json n1 = 0.9, n2 = 0.8, n3 = 3, s = "5";
  EXPECT_TRUE(when_holds(above, &n1));
  EXPECT_FALSE(when_holds(above, &n2));                            // strict >
  EXPECT_TRUE(when_holds(below, &n3));
  EXPECT_FALSE(when_holds(above, &s));                             // non-number -> false, no throw
  EXPECT_FALSE(when_holds(above, nullptr));
}
